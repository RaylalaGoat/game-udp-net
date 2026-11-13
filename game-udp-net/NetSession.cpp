// ==============================================================
// NetSession.cpp 
//
// 【このファイルの役割】
//   NetSession.h で宣言した関数の中身。通信モジュールの「頭脳」。
//   ・参加の受付、参加者一覧、試合の開始・終了 (ロビー)
//   ・状態の同期と補間
//   ・確実に届ける通信 (通し番号・返事・送り直し・順番の並べ直し)
//   ・切断の判定
//   Winsock は一切使わない。送受信は全部 transport_ (ITransport) 経由。
//
// 【大きな流れ】
//   ゲームが毎フレーム Update() を呼ぶ
//     → ReceivePackets() : 届いたパケットを全部読む → HandlePacket() でヘッダを確かめる
//          → ホストなら HandleHostPacket()、参加者なら HandleClientPacket() へ
//     → ホストなら UpdateHost()、参加者なら UpdateClient()
//          → タイムアウトの判定、状態の送信、確実なメッセージの送信・送り直し
// ==============================================================

#include "NetSession.h"   // 自分のヘッダを先に (ヘッダが単独でコンパイルできるかの確認にもなる)
#include "NetProtocol.h"  // パケットの形と ByteWriter / ByteReader (ゲーム側には見せない内部のもの)

#include <algorithm> // std::min, std::max
#include <chrono>    // std::chrono::steady_clock (時計)
#include <cmath>     // std::sqrt, std::isfinite
#include <random>    // std::mt19937, std::random_device (乱数)
#include <utility>   // std::move

namespace net
{
    // using 宣言: detail::ByteReader を、このファイルの中では ByteReader と短く書けるようにする。
    // using namespace detail; と全部を取り込まず、使うものだけを 1 つずつ書く
    //   → どの名前がどこから来たかが分かり、思わぬ名前の衝突も起きない。
    using detail::ByteReader;
    using detail::ByteWriter;
    using detail::MsgType;
    using detail::RefuseReason;
    using detail::ReliableKind;

    // ==========================================================
    // 内部の道具 (このファイルの中だけで使う定数と関数)
    // ==========================================================
    // 無名の名前空間: 中のものは、このファイルの外から見えない (他の .cpp と名前がぶつからない)。
    namespace
    {
        // パケット 1 個分の入れ物 (1200 バイト)。std::array なのでスタックに置かれ、ヒープの確保が起きない。
        using PacketBuffer = std::array<std::uint8_t, detail::kMaxPacketSize>;

        // ------------------------------------------------------
        // 1: 定数
        // ------------------------------------------------------
        // 【上限がたくさんある理由】
        //   ネットワークの向こうは信用できない。悪意のある人や壊れた相手が大量のパケットを送ってきても、
        //   メモリを使い果たしたり、1 フレームが止まったりしないよう、全ての「たまるもの」に上限を付けている。
        constexpr int         kHeaderSize = 8;    // magic(2) + version(1) + type(1) + token(4)
        constexpr int         kMaxPacketsPerUpdate = 1024; // 1回の Update で読む上限 (大量に来てもフレームが止まらない)
        constexpr std::size_t kMaxPendingReliable = 1024; // 送信待ちの上限 (超えたら SendGameEvent が false)
        constexpr std::size_t kMaxOutOfOrder = 64;   // 順番待ちで預かる上限
        constexpr int         kSendWindow = 64;   // 返事を待たずに送ってよい確実なメッセージの数
        // (相手が預かれる数 kMaxOutOfOrder と同じにしてある。
        //  これより先を送っても相手が捨てるだけで、無駄な送り直しが増えるため)
        constexpr int         kMaxReliablePacketsPerFlush = 4;  // 1回の送信で使うパケット数の上限
        constexpr int         kAckMaskBits = 32;   // 先の番号を何個分まで返事に含めるか
        constexpr std::size_t kMaxEvents = 1024; // 取り出されていない出来事の上限
        // (ゲームが PollEvent を呼び忘れても、メモリが増え続けない)
        constexpr int         kMaxReliableBodySize = 512;  // 確実なメッセージ 1 つの中身の上限
        constexpr int         kLeaveRepeatCount = 3;    // Leave は消えても困らないよう3回送る
        // std::size_t と int が混ざっているのは、比べる相手に合わせているため。
        //   pending.size() (size_t) と比べるものは size_t、ループの回数やバイト数 (int) と比べるものは int。
        //   符号付きと符号なしを比べると警告が出たり、負の数が巨大な数として比べられたりする事故があるので、そろえている。

        // ------------------------------------------------------
        // 2: 時計と乱数
        // ------------------------------------------------------
        // 今の時刻を秒で返す。NetSession のコンストラクタで時計を渡されなかったときに使う。
        //   steady_clock : 「必ず進む」時計。PC の時刻を変えても、時刻合わせが走っても戻らない。
        //   (system_clock はカレンダーの時刻なので、手で変えられると時間が戻り、タイムアウトの計算が壊れる)
        //   time_since_epoch() : 基準の時点からの経過時間。基準が何かは決まっていないが、差を取るだけなので問題ない。
        //   duration<double> : 秒を double で表す型に変換し、count() で数値を取り出す。
        double SteadyNowSeconds()
        {
            using Clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
        }

        // 0 以外の乱数 (セッション番号・参加申し込み番号に使う)
        //   参加申し込みの番号 (nonce) と、接続ごとの合言葉 (token) に使う。
        std::uint32_t MakeRandomId()
        {
            // static のローカル変数: 最初に呼ばれたときに 1 回だけ作られ、以後は同じものを使い続ける。
            //   (C++11 から、static ローカル変数の初期化はスレッドが複数あっても 1 回だけと保証されている)
            // std::mt19937 : メルセンヌ・ツイスタという乱数の作り方。速くて質が良い。
            // 種 (seed) は 2 つを ^ (XOR) で混ぜる:
            //   ・std::random_device{}() : OS が用意する「予測しにくい」乱数
            //   ・今の時刻 : random_device が毎回同じ値を返す古い環境 (昔の MinGW など) への保険
            //   同じ PC で 2 つ起動しても、別々の番号になるようにしている。
            // 注意: mt19937 は暗号用ではない (出力を集めると次が予想できる)。
            //       ここでは「接続を見分ける」ためだけに使っていて、不正を防ぐ鍵ではない。
            static std::mt19937 engine(
                std::random_device{}() ^
                static_cast<std::uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
            const auto value = static_cast<std::uint32_t>(engine());
            // 0 は「まだ決まっていない」の印に使っているので、乱数が偶然 0 になったら 1 にする。
            // 1u の u は unsigned (符号なし) の数という意味。value と型をそろえる。
            return (value == 0) ? 1u : value;
        }

        // ------------------------------------------------------
        // 3: 値のチェック
        // ------------------------------------------------------
        // パケットから読んだ ID や陣営が正しい範囲か確かめる。
        // 引数を int にしているのは、uint8_t で受けると 255 などの大きな値も「正しい型」として通ってしまうのと、
        // 比べる相手 (kMaxPlayers など) が int だから。
        bool IsValidId(int id) { return id >= 0 && id < kMaxPlayers; }
        // Team の最後の値 (Squad) と比べる。Team に値を足したら、最後の値をここで使うように直す必要がある。
        bool IsValidTeam(int team) { return team >= 0 && team <= static_cast<int>(Team::Squad); }

        // ------------------------------------------------------
        // 4: パケットの読み書き
        // ------------------------------------------------------
        // token: 接続ごとの合言葉。違うものは古い接続のパケットとして捨てる
        // 全てのパケットの先頭 8 バイトを書く。どの種類のパケットも、必ず最初にこれを呼ぶ。
        void WriteHeader(ByteWriter& w, MsgType type, std::uint32_t token)
        {
            w.U16(detail::kProtocolMagic);                // 2 バイト: このゲームの印
            w.U8(detail::kProtocolVersion);               // 1 バイト: 取り決めのバージョン
            w.U8(static_cast<std::uint8_t>(type));        // 1 バイト: パケットの種類 (enum class は static_cast で数に戻す)
            w.U32(token);                                 // 4 バイト: 接続の合言葉
        }

        // 1 人分の状態を書く。ClientState と WorldState の両方で使う。
        //   位置 3 × 4 バイト + 回転 4 × 4 バイト + custom の長さ 1 バイト + custom の中身 = 29 + custom
        void WriteState(ByteWriter& w, const PlayerState& s)
        {
            // 範囲 for 文: 配列の全ての要素を順番に v に入れて回す。添え字の書き間違いが起きない。
            for (float v : s.transform.position) { w.F32(v); }
            for (float v : s.transform.rotation) { w.F32(v); }

            // customSize が壊れていても (例: ゲーム側が直接 200 を入れた) 64 を超えて書かないように、小さい方を使う。
            //   std::min<std::uint8_t> : 2 つの引数を uint8_t にそろえて比べる。
            //   (std::min は 2 つの引数が同じ型でないとコンパイルエラーになるので、型を明示している)
            const std::uint8_t size = std::min<std::uint8_t>(s.customSize, kMaxCustomStateSize);
            w.U8(size);                       // 先に長さを書き、
            w.Bytes(s.custom.data(), size);   // その長さだけ中身を書く (使っていない部分は送らない → 通信量の節約)
        }

        // 1 人分の状態を読む。WriteState と全く同じ順番で読む。
        // 戻り値 false = 壊れたデータ。呼んだ側はそのパケットを捨てる。
        bool ReadState(ByteReader& r, PlayerState& s)
        {
            // float& v : 参照で受けるので、v に代入すると配列の要素そのものが書き換わる。
            for (float& v : s.transform.position) { v = r.F32(); }
            for (float& v : s.transform.rotation) { v = r.F32(); }
            const std::uint8_t size = r.U8();
            // 長さは外から来た値なので、必ず上限と比べる。
            // 比べないと、s.custom (64 バイト) の外に書き込む = バッファオーバーラン (未定義動作・乗っ取りの原因) になる。
            if (!r.Ok() || size > kMaxCustomStateSize)
            {
                return false;
            }
            s.customSize = size;
            r.Bytes(s.custom.data(), size);
            if (!r.Ok())
            {
                return false;
            }

            // NaN や無限大は受け付けない (描画や物理が壊れるため)
            //   NaN (Not a Number) は 0/0 などで生まれる「数ではない値」。
            //   一度混ざると、足しても掛けても NaN になり、行列が全部 NaN → キャラが消える・カメラが壊れる。
            //   壊れたパケットや改造したクライアントから来ることがあるので、入口で止める。
            //   std::isfinite(v) : v が普通の数 (NaN でも無限大でもない) なら true。
            for (float v : s.transform.position) { if (!std::isfinite(v)) { return false; } }
            for (float v : s.transform.rotation) { if (!std::isfinite(v)) { return false; } }
            return true;
        }

        // ------------------------------------------------------
        // 5: 補間の計算
        // ------------------------------------------------------
        // 線形補間 (Lerp): a と b の間を t (0〜1) の割合で求める。
        //   t = 0 → a、t = 1 → b、t = 0.5 → ちょうど真ん中。
        //   a * (1 - t) + b * t と同じ意味だが、この形だと t = 1 のとき誤差なく b に近い値になりやすい。
        float Lerp(float a, float b, float t) { return a + (b - a) * t; }

        // 位置は線形補間、向きはクォータニオンの正規化線形補間 (nlerp)
        //   nlerp : 4 つの数をそれぞれ Lerp して、長さを 1 に直す (正規化) だけの簡単な方法。
        //   正確な球面線形補間 (slerp) より速く、30 回/秒で届く近い向きの間なら見た目の差はほとんどない。
        Transform InterpolateTransform(const Transform& a, const Transform& b, float t)
        {
            Transform out;
            for (int i = 0; i < 3; ++i)
            {
                out.position[i] = Lerp(a.position[i], b.position[i], t);
            }

            // q と -q は同じ向き。内積が負なら片方を反転して、遠回りの回転を防ぐ
            //   クォータニオンでは (x, y, z, w) と (-x, -y, -z, -w) が同じ向きを表す。
            //   そのまま補間すると、10 度回せばよいところを 350 度回る「遠回り」になることがある。
            //   内積 (4 つの掛け算の合計) が負なら「反対側にある」ので、b の符号を反転してから補間する。
            float dot = 0.0f;
            for (int i = 0; i < 4; ++i) { dot += a.rotation[i] * b.rotation[i]; }
            const float sign = (dot < 0.0f) ? -1.0f : 1.0f;

            float lengthSq = 0.0f; // 長さの 2 乗 (平方根を取る前の値)
            for (int i = 0; i < 4; ++i)
            {
                out.rotation[i] = Lerp(a.rotation[i], b.rotation[i] * sign, t);
                lengthSq += out.rotation[i] * out.rotation[i];
            }

            // Lerp した結果は長さが 1 より少し短くなるので、長さで割って 1 に直す (正規化)。
            // 長さがほぼ 0 だと割り算が壊れる (0 除算・巨大な値) ので、そのときは a の向きをそのまま使う。
            //   1e-12f = 0.000000000001 (float の f)。「ほぼ 0」の判定に使う小さい数。
            if (lengthSq > 1e-12f)
            {
                const float inv = 1.0f / std::sqrt(lengthSq); // 割り算を 4 回するより、逆数を 1 回求めて掛ける方が速い
                for (float& v : out.rotation) { v *= inv; }
            }
            else
            {
                for (int i = 0; i < 4; ++i) { out.rotation[i] = a.rotation[i]; }
            }
            return out;
        }
    }

    // ==========================================================
    // SnapshotBuffer (補間のために、届いた状態を時刻つきで貯める)
    // ==========================================================
    // 関数名の NetSession::SnapshotBuffer:: は「NetSession の中の SnapshotBuffer の関数」という意味。
    // (入れ子のクラスの関数を .cpp に書くときは、外側のクラス名から全部書く)

    // 中身を空にする。head と count を 0 にするだけで、items は消さない (次の Push で上書きされる)。
    // 補足: 今のコードでは使われていない (Peer ごと作り直しているため)。
    void NetSession::SnapshotBuffer::Clear()
    {
        head = 0;
        count = 0;
    }

    // 新しい状態を時刻つきで足す。
    void NetSession::SnapshotBuffer::Push(double time, const PlayerState& state)
    {
        // 時刻が戻るものは入れない (古いパケット)
        //   UDP は順番が入れ替わって届くことがある。古い時刻のものを後ろに足すと、
        //   「時刻が順に並んでいる」前提が崩れて、補間で探すときに間違える。
        //   At(count - 1) = 一番新しいもの。
        if (count > 0 && time <= At(count - 1).time)
        {
            return;
        }
        // 満杯なら一番古いものを捨てる
        //   head を 1 つ進めるだけ。古い要素は消さず、次の書き込みで上書きされる。
        //   % kCapacity で、31 の次は 0 に戻る (輪になる)。
        if (count == kCapacity)
        {
            head = (head + 1) % kCapacity;
            --count;
        }
        // 書き込む場所 = 一番古いものから count 個進んだ場所 (= 一番新しいものの次)。
        // Snapshot& slot : 参照なので、slot に書くと items の中身が直接書き換わる (コピーしない)。
        Snapshot& slot = items[(head + count) % kCapacity];
        slot.time = time;
        slot.state = state;
        ++count;
    }

    // time (ホストの時計のミリ秒) の時点の状態を求める。
    //   その時刻をはさむ 2 つのスナップショットを探し、間を補間する。
    bool NetSession::SnapshotBuffer::Sample(double time, PlayerState& out) const
    {
        if (count == 0)
        {
            return false; // まだ何も届いていない
        }

        // 欲しい時刻が一番古いものより前 → 一番古いものを使う (それより前の情報はない)
        const Snapshot& oldest = At(0);
        if (time <= oldest.time)
        {
            out = oldest.state;
            return true;
        }

        // 欲しい時刻が一番新しいものより後 → 一番新しいものを使う
        //   次のパケットが遅れているとき。ここで止まって見える (カクつきの原因)。
        //   「この先どう動くか」を予測する方法 (外挿) もあるが、外れると不自然に戻るので、ここではしない。
        const Snapshot& newest = At(count - 1);
        if (time >= newest.time)
        {
            out = newest.state; // 先の予測はしない
            return true;
        }

        // time をはさむ2つを新しい側から探す
        //   表示したい時刻は「少し前」なので、新しい側から探すとすぐ見つかる (たいてい 2〜4 個目)。
        for (int i = count - 1; i > 0; --i)
        {
            const Snapshot& older = At(i - 1);
            if (older.time <= time)
            {
                const Snapshot& newer = At(i);
                const double span = newer.time - older.time; // 2 つの間の時間
                // t = 2 つの間のどこにいるか (0〜1)。
                // span が 0 になることは Push で防いでいるが、念のため 0 除算を避ける (防御的プログラミング)。
                const float  t = (span > 0.0) ? static_cast<float>((time - older.time) / span) : 1.0f;
                out = older.state; // custom は補間できないので古い側を使う
                // (HP 100 と 90 の間で 95 にする、アニメ番号 3 と 4 の間で 3.5 にする、は意味がない)
                out.transform = InterpolateTransform(older.state.transform, newer.state.transform, t);
                return true;
            }
        }

        // ここには来ないはず (上の 2 つの if で、oldest < time < newest が保証されているため)。
        // それでも何か返すようにして、関数の終わりまで戻り値なしで落ちる事故を防いでいる。
        out = oldest.state;
        return true;
    }

    // ==========================================================
    // 生成・破棄
    // ==========================================================
    // コンストラクタ。: の後ろはメンバ初期化子リスト (ヘッダに宣言した順番と同じ順に書いている)。
    //   transport_ は参照なので、ここでしか初期化できない (後から代入できない)。
    //   clock_ : 時計が渡されていれば (clock が空でなければ) それを使い、なければ SteadyNowSeconds を使う。
    //            std::move(clock) : clock の中身を「移す」。std::function はコピーより移動の方が軽い。
    //            Clock(SteadyNowSeconds) : 普通の関数を std::function に包む。
    NetSession::NetSession(ITransport& transport, const SessionConfig& config, Clock clock)
        : transport_(transport)
        , config_(config)
        , clock_(clock ? std::move(clock) : Clock(SteadyNowSeconds))
    {
        // 0 以下だと「1.0 / stateSendRate」で 0 除算や負の間隔になり、毎フレーム送り続ける事故になる。
        // 外から渡される設定は、受け取った所で正しい値に直しておく。
        if (config_.stateSendRate <= 0.0)
        {
            config_.stateSendRate = 30.0;
        }
    }

    // デストラクタ。ゲームを終了したときに、相手に「抜けます」を送ってから消える。
    // これがないと、相手は 3 秒のタイムアウトまで「まだいる」と思い続ける。
    NetSession::~NetSession()
    {
        Leave();
    }

    // 全ての変数を最初の状態 (Idle) に戻す。Host / Join / Leave から呼ぶ。
    // 戻す変数を1か所にまとめておくと、変数を足したときに戻し忘れが起きにくい。
    // 補足: events_ はここでは消さない。Leave() の後でも、それまでに届いた出来事を取り出せるようにするため。
    void NetSession::ResetSession()
    {
        state_ = SessionState::Idle;
        isHost_ = false;
        localId_ = kInvalidPlayerId;
        connectionToken_ = 0;
        inMatch_ = false;
        matchNumber_ = 0;

        // 範囲 for 文 + 参照 (Peer&) で、配列の各要素そのものを初期値の Peer() で上書きする。
        // Peer() : 全メンバが初期値 (= 0、false など) の Peer を作る。
        for (Peer& peer : peers_)
        {
            peer = Peer();
        }

        nextStateSendTime_ = 0.0;
        stateSeq_ = 0;
        hostAddress_ = NetAddress();
        joinNonce_ = 0;
        joinStartTime_ = 0.0;
        lastJoinSendTime_ = 0.0;
        lastHostReceiveTime_ = 0.0;
        hostChannel_ = ReliableChannel(); // 中の deque と map も空になる
        hasWorldSeq_ = false;
        lastWorldSeq_ = 0;
        hasClockOffset_ = false;
        clockOffsetMs_ = 0.0;
    }

    // ==========================================================
    // 開始・終了
    // ==========================================================
    // ホストになる。
    bool NetSession::Host()
    {
        // 既に Connecting / Lobby / InMatch なら何もしない (二重に始めない)。
        if (state_ != SessionState::Idle && state_ != SessionState::Disconnected)
        {
            return false;
        }

        ResetSession();
        events_.clear();
        DrainTransport(); // 前のセッションの残りを捨てる
        // (前の試合のパケットがソケットに残っていると、新しいセッションで読んでしまうため)

        now_ = clock_();
        startTime_ = now_;              // ホストの時計の 0 地点。ここからの経過ミリ秒を状態に付けて送る
        isHost_ = true;
        localId_ = kHostPlayerId;     // ホストは必ず 0 番
        peers_[kHostPlayerId].active = true;
        state_ = SessionState::Lobby; // ホストは参加の手続きがないので、すぐ Lobby
        nextStateSendTime_ = now_;      // 最初の Update ですぐ送る

        // ゲーム側には参加者と同じく Connected で知らせる (ホストか参加者かで処理を分けなくてよい)。
        PushEvent(NetEventType::Connected, localId_);
        return true;
    }

    // ホストに参加を申し込む。結果はイベントで届く。
    bool NetSession::Join(const NetAddress& hostAddress)
    {
        if (state_ != SessionState::Idle && state_ != SessionState::Disconnected)
        {
            return false;
        }

        ResetSession();
        events_.clear();
        DrainTransport();

        now_ = clock_();
        startTime_ = now_;
        isHost_ = false;
        hostAddress_ = hostAddress;
        joinNonce_ = MakeRandomId(); // 今回の申し込みの番号。返事にこれが入っていれば、今回の申し込みへの返事
        joinStartTime_ = now_;           // 5 秒のタイムアウトを測り始める
        state_ = SessionState::Connecting;

        // 最初の申し込みはすぐ送る。届かなければ UpdateClient() が 0.25 秒ごとに送り直す。
        SendJoinRequest();
        lastJoinSendTime_ = now_;
        return true;
    }

    // 抜ける。何回呼んでもよい (2 回目以降は Idle なので何もしない)。
    void NetSession::Leave()
    {
        if (state_ == SessionState::Idle)
        {
            return;
        }

        // Disconnected (既に切れている) なら、相手はもういないので送らない。
        if (state_ != SessionState::Disconnected)
        {
            if (isHost_)
            {
                // ホスト: 参加者全員に「抜けます」を送る → 参加者側は HostLeft で切断される。
                // id = 1 から始めるのは、0 は自分 (ホスト) だから。
                for (PlayerId id = 1; id < kMaxPlayers; ++id)
                {
                    if (peers_[id].active)
                    {
                        SendLeave(peers_[id].address, peers_[id].token);
                    }
                }
            }
            else
            {
                // 参加者: ホストにだけ送る。ホストが他の参加者に PlayerLeft を伝える。
                // 補足: Connecting 中に呼ぶと、まだ合言葉 (token) をもらっていないので 0 で送ることになり、
                //       ホストには無視される。その場合、ホストが用意した席は 3 秒のタイムアウトで空く。
                SendLeave(hostAddress_, connectionToken_);
            }
        }

        // 自分で抜けたときは Disconnected ではなく Idle に戻す (イベントも出さない)。
        ResetSession();
    }

    // ==========================================================
    // 毎フレームの処理
    // ==========================================================
    // ゲームが毎フレーム呼ぶ。通信モジュールが動くのはこの中だけ。
    void NetSession::Update()
    {
        // このフレームの時刻を 1 回だけ取る。
        // 関数の中で何度も clock_() を呼ぶと、少しずつ違う時刻になり、判定がずれることがあるため。
        now_ = clock_();
        if (state_ == SessionState::Idle || state_ == SessionState::Disconnected)
        {
            return; // 何もしていない / 切れている
        }

        ReceivePackets();
        // 受信の途中で切断されることがある (ホストが抜けた、エラー、など)。
        // そのときは、もう送る相手がいないので、ここで終わる。
        if (state_ == SessionState::Idle || state_ == SessionState::Disconnected)
        {
            return;
        }

        // 受信の後に送信する。受け取った返事を先に処理してから送ると、
        // 「もう届いているメッセージ」を送り直す無駄が減る。
        if (isHost_)
        {
            UpdateHost();
        }
        else
        {
            UpdateClient();
        }
    }

    // 届いているパケットを読んで、中身を見ずに全部捨てる (Host / Join の開始時)。
    void NetSession::DrainTransport()
    {
        PacketBuffer buffer;
        // 無限ループにしないため、上限 (1024 × 4 個) を付ける。
        // 相手が送り続けていると、いつまでも 0 にならない可能性があるため。
        for (int i = 0; i < kMaxPacketsPerUpdate * 4; ++i)
        {
            NetAddress from;
            if (transport_.ReceiveFrom(buffer.data(), static_cast<int>(buffer.size()), from) <= 0)
            {
                return; // 何も残っていない (0) か、エラー (-1)
            }
        }
    }

    // 届いているパケットを、1 回の Update で最大 1024 個読む。
    void NetSession::ReceivePackets()
    {
        // PacketBuffer はループの外で 1 回だけ作り、使い回す (毎回作るより無駄がない)。
        PacketBuffer buffer;
        for (int i = 0; i < kMaxPacketsPerUpdate; ++i)
        {
            NetAddress from;
            // buffer.size() は size_t (符号なし)。ITransport は int を受け取るので static_cast で合わせる。
            const int n = transport_.ReceiveFrom(buffer.data(), static_cast<int>(buffer.size()), from);
            if (n == 0)
            {
                return; // もう何も届いていない (普通の終わり方)
            }
            if (n < 0)
            {
                // 送受信の層が壊れた。続けても何も届かないので切断する。
                Disconnect(DisconnectReason::TransportError);
                return;
            }

            HandlePacket(from, buffer.data(), n);
            // パケットの処理で切断されたら (例: ホストから Leave が届いた)、残りは読まない。
            if (state_ == SessionState::Idle || state_ == SessionState::Disconnected)
            {
                return;
            }
        }
    }

    // 1 個のパケットのヘッダを確かめ、ホスト用 / 参加者用の処理に振り分ける。
    void NetSession::HandlePacket(const NetAddress& from, const std::uint8_t* data, int size)
    {
        ByteReader r(data, size);
        // WriteHeader と同じ順番で読む。
        const std::uint16_t magic = r.U16();
        const std::uint8_t  version = r.U8();
        const std::uint8_t  type = r.U8();
        const std::uint32_t token = r.U32();
        // 8 バイトより短い (r.Ok() が false)、印が違う、バージョンが違う → このゲームのパケットではないので捨てる。
        // 黙って捨てる (エラーにしない) のは、知らないパケットが届くのは UDP では普通のことだから。
        if (!r.Ok() || magic != detail::kProtocolMagic || version != detail::kProtocolVersion)
        {
            return;
        }

        // ヘッダの後ろが本体。r.Ok() が true なので size は 8 以上 → bodySize は 0 以上。
        const std::uint8_t* body = data + kHeaderSize;
        const int           bodySize = size - kHeaderSize;
        // type と token はまだ確かめていない。正しいかどうかは、ホスト / 参加者で判断が違うので、それぞれに任せる。
        if (isHost_)
        {
            HandleHostPacket(from, type, token, body, bodySize);
        }
        else
        {
            HandleClientPacket(from, type, token, body, bodySize);
        }
    }

    // 状態を送る時刻になったか。なっていたら次の時刻を決めて true を返す。
    bool NetSession::IsStateDue()
    {
        if (now_ < nextStateSendTime_)
        {
            return false;
        }
        const double interval = 1.0 / config_.stateSendRate; // 30 回/秒なら 0.0333... 秒
        // 「今 + 間隔」ではなく「前の予定 + 間隔」にする。
        //   例: 予定 0.033 秒、実際のフレームが 0.040 秒に来た。
        //       今 + 間隔 だと次は 0.073 秒 → 遅れがたまり、毎秒 30 回より少なくなる。
        //       前の予定 + 間隔 だと次は 0.067 秒 → 遅れを取り戻し、平均で正確に 30 回になる。
        nextStateSendTime_ += interval;
        if (nextStateSendTime_ < now_)
        {
            // ただし、大きく遅れたとき (ゲームが 1 秒止まった、など) に、遅れた分を一気に送ろうとすると
            // 何十回分も連続で送ってしまうので、そのときは今を基準に仕切り直す。
            nextStateSendTime_ = now_ + interval; // 大きく遅れたら追いつこうとしない
        }
        return true;
    }

    // Host / Join してからの経過時間 (ミリ秒)。状態に付ける時刻として使う。
    // std::max(0.0, ...) : 万が一マイナスになっても 0 にする (時計を差し替えたテストなどへの保険)。
    double NetSession::ElapsedMs() const
    {
        return std::max(0.0, (now_ - startTime_) * 1000.0);
    }

    // 補間で表示する時刻 (ホストの時計で、何ミリ秒の時点を表示するか)。
    double NetSession::RenderTimeMs() const
    {
        const double delayMs = config_.interpolationDelay * 1000.0; // 0.1 秒 → 100 ミリ秒
        if (isHost_)
        {
            // ホストは自分の時計がそのまま基準の時計。
            return ElapsedMs() - delayMs;
        }
        // 自分の時計をホストの時計に換算し、少し過去を表示する
        //   参加者は Join した時刻が 0 なので、ホストの時計とずれている。
        //   clockOffsetMs_ (自分の時計 − ホストの時計) を引けば、ホストの時計に直せる (UpdateClockOffset で推定)。
        return ElapsedMs() - clockOffsetMs_ - delayMs;
    }

    // ==========================================================
    // ホスト: 受信と参加の受付
    // ==========================================================
    // ホストに届いたパケットを処理する。
    // 参加の申し込み以外は、3 つの関門を全て通ったパケットだけを処理する:
    //   関門1: 知っている住所か      (FindPeerByAddress)
    //   関門2: 合言葉が合っているか  (token)
    //   関門3: 名乗った ID が本当か   (claimed == id)
    void NetSession::HandleHostPacket(const NetAddress& from, std::uint8_t type, std::uint32_t token,
        const std::uint8_t* body, int size)
    {
        // 参加の申し込みだけは、まだ住所も合言葉も登録されていないので、関門の前に別の関数で扱う。
        if (type == static_cast<std::uint8_t>(MsgType::JoinRequest))
        {
            HandleJoinRequest(from, body, size);
            return;
        }

        // 関門1: 送り主の住所から、どの席の参加者かを探す。
        const PlayerId id = FindPeerByAddress(from);
        if (id == kInvalidPlayerId)
        {
            return; // 知らない相手
        }
        // 関門2: 住所が同じでも、ゲームを再起動して入り直した場合は合言葉が新しくなっている。
        //        古い合言葉のパケット (再起動前のゲームが最後に送ったもの) は捨てる。
        if (token != peers_[id].token)
        {
            return; // 同じ住所の古い接続 (再起動前のゲーム) のパケット
        }

        // 関門3: 本体の先頭 1 バイトは「送り主の ID」。住所から分かった ID と違えば、他人のふり。
        ByteReader r(body, size);
        const PlayerId claimed = r.U8();
        if (!r.Ok() || claimed != id)
        {
            return; // 他人のふり
        }

        // ここまで来たら正しい相手。何か届いたので、タイムアウトの時計をリセットする。
        // Peer& peer : 参照で受けて、peers_[id] を毎回書かずに済むようにする (書き換えも元に反映される)。
        Peer& peer = peers_[id];
        peer.lastReceiveTime = now_;

        // type は uint8_t で届いたので、MsgType に戻して switch する。
        // 範囲外の値 (例: 99) でも、どの case にも当たらず default に行くだけなので安全。
        switch (static_cast<MsgType>(type))
        {
        case MsgType::Leave:
            RemovePlayer(id, DisconnectReason::Left); // 参加者が自分から抜けた
            break;

        case MsgType::ClientState:
        {
            // case の中で変数を作るときは { } で囲む。
            // 囲まないと、次の case に飛んだときに「初期化を飛び越えた」というコンパイルエラーになる。
            const std::uint32_t seq = r.U32();
            PlayerState state;
            if (!ReadState(r, state))
            {
                break; // 壊れたデータ
            }
            // 順番が入れ替わって届いた古い状態や、重複して届いた同じ状態は使わない。
            if (peer.hasState && !detail::SeqNewer32(seq, peer.lastStateSeq))
            {
                break; // 古い or 重複
            }
            peer.hasState = true;
            peer.lastStateSeq = seq;
            peer.latestState = state;   // WorldState で他の参加者に配る用
            // ホストが補間に使う用。時刻は「ホストが受け取った時刻」(ホストの時計) を付ける。
            // 参加者の時計はホストとずれているので、ホストの時計にそろえて貯める。
            peer.snapshots.Push(ElapsedMs(), state);
            break;
        }

        case MsgType::Reliable:
            // body + 1, size - 1 : 先頭の「送り主 ID」1 バイトを飛ばした残り。
            ReceiveReliable(peer.channel, id, body + 1, size - 1);
            break;

        case MsgType::Ack:
        {
            const std::uint16_t next = r.U16();  // 「次は何番が欲しい」
            const std::uint32_t mask = r.U32();  // 「その先の何番が届いた」
            if (r.Ok())
            {
                HandleAck(peer.channel, next, mask);
            }
            break;
        }

        default:
            break; // 参加者からホストに来るはずのない種類 (WorldState など) は無視
        }
    }

    // 参加の申し込みを処理する。
    //   本体: 送り主 ID (1, まだ無効) + 申し込み番号 nonce (4)
    //   返事: JoinAccept (ID を割り当てた) か JoinRefuse (断る)
    void NetSession::HandleJoinRequest(const NetAddress& from, const std::uint8_t* body, int size)
    {
        ByteReader r(body, size);
        r.U8(); // 送り主 ID (まだ割り当てられていない)   ← 読み飛ばすために読むだけ (戻り値は使わない)
        const std::uint32_t nonce = r.U32();
        if (!r.Ok())
        {
            return;
        }

        // ラムダ式: 関数の中で作る小さな関数。返事を送る処理を 3 か所で使うので、1 か所にまとめている。
        //   [&] : 外の変数 (from、nonce、this) を参照で使える (キャプチャ)。
        //         このラムダはこの関数の中でしか呼ばないので、参照でキャプチャしても寿命の問題がない。
        //   auto reply : ラムダの型は名前がない (コンパイラが作る) ので、auto で受ける。
        auto reply = [&](MsgType type, std::uint8_t value, std::uint32_t token)
            {
                PacketBuffer buffer;
                ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
                WriteHeader(w, type, token);  // JoinAccept なら新しい合言葉をヘッダに入れて渡す
                w.U8(kHostPlayerId);          // 送り主 = ホスト
                w.U32(nonce);                 // どの申し込みへの返事か (参加者はこれで古い返事を見分ける)
                w.U8(value);                  // JoinAccept なら割り当てた ID、JoinRefuse なら断る理由
                if (w.Ok())
                {
                    SendPacket(from, w.Data(), w.Size());
                }
            };

        // 同じ住所の人が既に参加しているか。
        PlayerId id = FindPeerByAddress(from);
        if (id != kInvalidPlayerId)
        {
            if (peers_[id].joinNonce == nonce)
            {
                // 返事が届かなかったので申し込みが繰り返された → 同じ返事を送る
                //   (JoinAccept が途中で消えると、参加者は 0.25 秒後に同じ nonce で申し込み直してくる。
                //    新しい席を作らず、同じ ID と同じ合言葉で返すことで、二重に参加させない)
                peers_[id].lastReceiveTime = now_;
                reply(MsgType::JoinAccept, id, peers_[id].token);
                return;
            }
            // 同じ住所から新しい申し込み = 相手が再起動した。抜けた扱いにして入れ直す
            //   (nonce は Join() のたびに新しい乱数になるので、違う nonce = 別の申し込み)
            RemovePlayer(id, DisconnectReason::Left);
        }

        // 試合中は、設定で許していなければ断る。断るときは合言葉を渡さないので token は 0。
        if (inMatch_ && !config_.allowJoinInMatch)
        {
            reply(MsgType::JoinRefuse, static_cast<std::uint8_t>(RefuseReason::InMatch), 0);
            return;
        }

        // 空いている席を探す。なければ満員。
        id = FindFreeSlot();
        if (id == kInvalidPlayerId)
        {
            reply(MsgType::JoinRefuse, static_cast<std::uint8_t>(RefuseReason::Full), 0);
            return;
        }

        // 席を用意する。前に座っていた人の情報が残らないよう、まず Peer() で全部消してから設定する。
        Peer& peer = peers_[id];
        peer = Peer();
        peer.active = true;
        peer.address = from;
        peer.joinNonce = nonce;          // 同じ申し込みが繰り返されたとき用に覚えておく
        peer.token = MakeRandomId(); // この接続だけの合言葉
        peer.lastReceiveTime = now_;

        reply(MsgType::JoinAccept, id, peer.token);
        PushEvent(NetEventType::PlayerJoined, id); // ホストのゲームに知らせる
        BroadcastRoster();                         // 参加者全員 (新しい人も含む) に新しい一覧を配る
    }

    // ==========================================================
    // ホスト: 送信・退出・ロビー
    // ==========================================================
    // ホストの毎フレームの処理 (Update から呼ばれる)。
    void NetSession::UpdateHost()
    {
        // 切断の検出
        //   UDP には「切れた」という知らせがないので、「一定時間何も届かない = 切れた」と自分で決める。
        //   参加者は毎秒 30 回状態を送ってくるので、3 秒何も来ないのは明らかにおかしい。
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active && now_ - peers_[id].lastReceiveTime > config_.timeoutSeconds)
            {
                RemovePlayer(id, DisconnectReason::Timeout);
            }
        }

        // 時刻になったら、全員分の状態を配る。
        if (IsStateDue())
        {
            SendWorldState();
        }

        // 参加者ごとに、確実なメッセージを送る・送り直す・返事をする。
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active)
            {
                FlushReliable(peers_[id].channel, peers_[id].address, peers_[id].token);
            }
        }
    }

    // 全員分の状態を、参加者 1 人ずつに送る。
    //   パケットの形: ヘッダ + 送り主 ID + 通し番号 + ホストの時刻 + 人数 + (ID + 状態) × 人数
    void NetSession::SendWorldState()
    {
        ++stateSeq_; // 通し番号。1 回の送信で全員に同じ番号を使う

        // ホストの時刻 (ミリ秒) を 4 バイトの整数にする。参加者はこれで補間と時計合わせをする。
        // double → uint64_t → uint32_t の順に変換している理由:
        //   double を直接 uint32_t にすると、範囲を超えたとき (約 49.7 日後) に未定義動作になる。
        //   一度 uint64_t にしてから uint32_t にすれば、範囲を超えても「一周する」だけで済む。
        const auto hostTime = static_cast<std::uint32_t>(static_cast<std::uint64_t>(ElapsedMs()));

        // 送り先ごとに、その人以外の状態をまとめる
        //   自分の状態は自分の PC が一番よく知っている (遅れていない) ので、送り返さない (通信量の節約)。
        for (PlayerId target = 1; target < kMaxPlayers; ++target)
        {
            const Peer& dest = peers_[target];
            if (!dest.active)
            {
                continue;
            }

            // その送り先に「id の状態を入れるか」を判断するラムダ。
            // 人数を数えるときと、書き込むときの 2 回使うので、同じ条件を 2 回書かずに済む。
            auto included = [&](PlayerId id)
                {
                    if (id == target)
                    {
                        return false; // 送り先本人は入れない
                    }
                    // ホストは常に入れる。参加者は、状態が一度でも届いている人だけ。
                    return id == kHostPlayerId || (peers_[id].active && peers_[id].hasState);
                };

            // 先に人数を数える (パケットに「何人分あるか」を書いてから中身を書くため)。
            std::uint8_t count = 0;
            for (PlayerId id = 0; id < kMaxPlayers; ++id)
            {
                if (included(id)) { ++count; }
            }

            PacketBuffer buffer;
            ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
            WriteHeader(w, MsgType::WorldState, dest.token); // 送り先ごとに合言葉が違う
            w.U8(kHostPlayerId);
            w.U32(stateSeq_);
            w.U32(hostTime);
            w.U8(count);
            for (PlayerId id = 0; id < kMaxPlayers; ++id)
            {
                if (!included(id))
                {
                    continue;
                }
                w.U8(id);
                // ホスト自身の状態は localState_、参加者の状態は最後に受け取ったもの。
                // (補間したものではなく、届いたままの値を送る。補間は受け取った側がする)
                WriteState(w, (id == kHostPlayerId) ? localState_ : peers_[id].latestState);
            }

            if (w.Ok())
            {
                SendPacket(dest.address, w.Data(), w.Size());
            }
        }
    }

    // 参加者を外す (抜けた・タイムアウト・再参加)。
    void NetSession::RemovePlayer(PlayerId id, DisconnectReason reason)
    {
        // ホスト自身 (0) や、空いている席は外さない。範囲外の ID も弾く。
        if (!IsValidId(id) || id == kHostPlayerId || !peers_[id].active)
        {
            return;
        }
        peers_[id] = Peer();                              // 席を空ける (送信待ちのメッセージなども全部消える)
        PushEvent(NetEventType::PlayerLeft, id, reason);  // ホストのゲームに知らせる
        BroadcastRoster();                                // 残りの参加者に新しい一覧を配る
    }

    // 参加者一覧 (Roster) を、参加者全員に「確実に届ける」通信で配る。
    //   中身: 種類 + 試合中か + 試合番号 + 人数 + (ID + 陣営) × 人数
    //
    // 【差分ではなく、毎回まるごと送る理由】
    //   「1 番が抜けた」「2 番が Solo になった」のような差分だと、1 つでも抜けたり、
    //   順番が狂ったりすると、参加者の一覧がずっと間違ったままになる。
    //   毎回まるごと送れば、最新の 1 つを受け取るだけで正しい一覧になる。
    //   4 人分でも 13 バイトほどなので、まるごと送っても負担にならない。
    //   (差分は受け取った側の ApplyRoster が計算して、イベントにする)
    void NetSession::BroadcastRoster()
    {
        // 最大でも 5 + 2 × 4 = 13 バイトなので、64 バイトの小さな入れ物で足りる。
        std::array<std::uint8_t, 64> buffer;
        ByteWriter w(buffer.data(), static_cast<int>(buffer.size()));
        w.U8(static_cast<std::uint8_t>(ReliableKind::Roster));
        w.U8(inMatch_ ? 1 : 0);
        w.U16(matchNumber_);

        std::uint8_t count = 0;
        for (const Peer& peer : peers_)
        {
            if (peer.active) { ++count; }
        }
        w.U8(count);
        for (PlayerId id = 0; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active)
            {
                w.U8(id);
                w.U8(static_cast<std::uint8_t>(peers_[id].team));
            }
        }
        if (!w.Ok())
        {
            return;
        }

        // vector に入れて、参加者ごとの送信待ちの列に足す (実際に送るのは FlushReliable)。
        // body(先頭, 終わり) : ポインタの範囲から vector を作るコンストラクタ。
        const std::vector<std::uint8_t> body(w.Data(), w.Data() + w.Size());
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active)
            {
                EnqueueReliable(peers_[id].channel, body);
            }
        }
    }

    // 住所から参加者の席を探す。ホスト (0) は自分なので探さない。
    // 最大 3 人なので、順番に見るだけで十分速い (表や map を使うより単純で、キャッシュにも優しい)。
    PlayerId NetSession::FindPeerByAddress(const NetAddress& address) const
    {
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active && peers_[id].address == address) // NetAddress::operator== を使う
            {
                return id;
            }
        }
        return kInvalidPlayerId;
    }

    // 空いている席を、小さい番号から探す。
    PlayerId NetSession::FindFreeSlot() const
    {
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (!peers_[id].active)
            {
                return id;
            }
        }
        return kInvalidPlayerId;
    }

    // ホストが陣営を決める (ロビー中だけ)。ホスト自身 (0) の陣営も決められる。
    bool NetSession::SetTeam(PlayerId id, Team team)
    {
        if (!isHost_ || state_ != SessionState::Lobby)
        {
            return false;
        }
        if (!IsValidId(id) || !peers_[id].active)
        {
            return false;
        }
        // 同じ陣営なら何もしない (無駄なイベントや送信を出さない)。成功扱いで true。
        if (peers_[id].team == team)
        {
            return true;
        }
        peers_[id].team = team;
        PushEvent(NetEventType::TeamChanged, id);
        BroadcastRoster();
        return true;
    }

    // 試合を始める。人数と陣営の条件を満たさないと false。
    bool NetSession::StartMatch()
    {
        if (!isHost_ || state_ != SessionState::Lobby)
        {
            return false;
        }

        // 1対1〜1対3: Solo がちょうど1人、Squad が1人以上、未設定がいない
        int solo = 0;
        int squad = 0;
        for (const Peer& peer : peers_)
        {
            if (!peer.active)
            {
                continue;
            }
            if (peer.team == Team::Solo) { ++solo; }
            else if (peer.team == Team::Squad) { ++squad; }
            else { return false; } // 陣営が決まっていない人がいる
        }
        if (solo != 1 || squad < 1)
        {
            return false;
        }

        inMatch_ = true;
        ++matchNumber_; // 試合ごとに番号を変える (参加者が「新しい試合が始まった」と分かるように)
        state_ = SessionState::InMatch;
        PushEvent(NetEventType::MatchStarted, kInvalidPlayerId); // 特定のプレイヤーに関係しないので無効 ID
        BroadcastRoster(); // 試合の開始も一覧に入れて配る (別のメッセージを作らない)
        return true;
    }

    // 試合を終えてロビーに戻る。
    bool NetSession::EndMatch()
    {
        if (!isHost_ || state_ != SessionState::InMatch)
        {
            return false;
        }
        inMatch_ = false;
        state_ = SessionState::Lobby;
        PushEvent(NetEventType::MatchEnded, kInvalidPlayerId);
        BroadcastRoster();
        return true;
    }

    // ==========================================================
    // 参加者: 受信と送信
    // ==========================================================
    // 参加者に届いたパケットを処理する。
    void NetSession::HandleClientPacket(const NetAddress& from, std::uint8_t type, std::uint32_t token,
        const std::uint8_t* body, int size)
    {
        // 参加者が話す相手はホストだけ。他の住所からのパケットは全部捨てる。
        if (from != hostAddress_)
        {
            return; // ホスト以外からは受け取らない
        }

        ByteReader r(body, size);
        const PlayerId sender = r.U8();
        if (!r.Ok() || sender != kHostPlayerId)
        {
            return;
        }
        const auto msg = static_cast<MsgType>(type);

        //------------------------------------------------------
        // 参加の返事待ち (Connecting) のときは、返事 (JoinAccept / JoinRefuse) だけを見る
        //------------------------------------------------------
        if (state_ == SessionState::Connecting)
        {
            if (msg != MsgType::JoinAccept && msg != MsgType::JoinRefuse)
            {
                return;
            }
            const std::uint32_t nonce = r.U32();
            const std::uint8_t  value = r.U8();
            // 今回の申し込みの番号でなければ、前の申し込み (前回の Join) への遅れた返事なので捨てる。
            if (!r.Ok() || nonce != joinNonce_)
            {
                return; // 前の申し込みへの返事
            }

            if (msg == MsgType::JoinRefuse)
            {
                // 三項演算子 (条件 ? A : B) で、断られた理由を DisconnectReason に変換する。
                Disconnect(value == static_cast<std::uint8_t>(RefuseReason::InMatch)
                    ? DisconnectReason::JoinRefusedInMatch
                    : DisconnectReason::JoinRefusedFull);
                return;
            }

            // 割り当てられた ID が正しい範囲か (ホストの 0 は参加者に割り当てられないはず)。
            if (!IsValidId(value) || value == kHostPlayerId)
            {
                return;
            }
            localId_ = value;
            connectionToken_ = token;       // ヘッダに入っていた合言葉を覚える。以後の全てのパケットに付ける
            peers_[localId_].active = true;
            state_ = SessionState::Lobby;
            lastHostReceiveTime_ = now_;    // タイムアウトの時計を始める
            nextStateSendTime_ = now_;    // すぐに自分の状態を送り始める
            PushEvent(NetEventType::Connected, localId_);
            return;
        }

        //------------------------------------------------------
        // 参加した後
        //------------------------------------------------------
        // 合言葉が違う = 前の接続 (前回の Join) のパケット。捨てる。
        if (token != connectionToken_)
        {
            return; // 前の接続のパケット
        }
        lastHostReceiveTime_ = now_; // ホストから何か届いたので、タイムアウトの時計をリセット

        switch (msg)
        {
        case MsgType::Leave:
            Disconnect(DisconnectReason::HostLeft); // ホストが抜けた → 自分も切断
            break;

        case MsgType::WorldState:
            HandleWorldState(body + 1, size - 1);   // 先頭の送り主 ID を飛ばして渡す
            break;

        case MsgType::Reliable:
            ReceiveReliable(hostChannel_, kHostPlayerId, body + 1, size - 1);
            break;

        case MsgType::Ack:
        {
            const std::uint16_t next = r.U16();
            const std::uint32_t mask = r.U32();
            if (r.Ok())
            {
                HandleAck(hostChannel_, next, mask);
            }
            break;
        }

        default:
            break; // JoinAccept の重複などは無視
            // (返事が 2 回届くことがある。2 回目は既に Lobby なので、ここで無視される)
        }
    }

    // 参加者の毎フレームの処理 (Update から呼ばれる)。
    void NetSession::UpdateClient()
    {
        // 返事待ちの間: 5 秒であきらめる。それまでは 0.25 秒ごとに申し込みを送り直す。
        if (state_ == SessionState::Connecting)
        {
            if (now_ - joinStartTime_ > config_.joinTimeoutSeconds)
            {
                Disconnect(DisconnectReason::JoinTimeout);
                return;
            }
            if (now_ - lastJoinSendTime_ >= config_.joinRetryInterval)
            {
                SendJoinRequest(); // 同じ nonce で送り直す (ホストは同じ申し込みだと分かる)
                lastJoinSendTime_ = now_;
            }
            return;
        }

        // 参加した後: ホストから 3 秒何も届かなければ切断。
        if (now_ - lastHostReceiveTime_ > config_.timeoutSeconds)
        {
            Disconnect(DisconnectReason::Timeout);
            return;
        }

        if (IsStateDue())
        {
            SendClientState();
        }
        FlushReliable(hostChannel_, hostAddress_, connectionToken_);
    }

    // 参加の申し込みを送る。まだ合言葉がないので token は 0、ID もまだないので無効 ID。
    void NetSession::SendJoinRequest()
    {
        PacketBuffer buffer;
        ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
        WriteHeader(w, MsgType::JoinRequest, 0);
        w.U8(kInvalidPlayerId);
        w.U32(joinNonce_);
        if (w.Ok())
        {
            SendPacket(hostAddress_, w.Data(), w.Size());
        }
    }

    // 自分の状態をホストに送る。
    void NetSession::SendClientState()
    {
        PacketBuffer buffer;
        ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
        WriteHeader(w, MsgType::ClientState, connectionToken_);
        w.U8(localId_);
        w.U32(++stateSeq_); // 前置インクリメント: 1 増やしてから、その値を書く
        WriteState(w, localState_);
        if (w.Ok())
        {
            SendPacket(hostAddress_, w.Data(), w.Size());
        }
    }

    // ==========================================================
    // 参加者: 全員の状態・時計・参加者一覧
    // ==========================================================
    // ホストから届いた全員分の状態を処理する。
    void NetSession::HandleWorldState(const std::uint8_t* data, int size)
    {
        ByteReader r(data, size);
        const std::uint32_t seq = r.U32();
        const std::uint32_t hostTime = r.U32();
        const std::uint8_t  count = r.U8();
        // 人数が 4 を超えていたら壊れたデータ (下の配列 ids / states からはみ出すのを防ぐ)。
        if (!r.Ok() || count > kMaxPlayers)
        {
            return;
        }
        if (hasWorldSeq_ && !detail::SeqNewer32(seq, lastWorldSeq_))
        {
            return; // 古い or 重複
        }

        // 全員分を読み終えて問題がなければ反映する (途中で壊れていたら何も変えない)
        // 読みながら反映すると、3 人目で壊れていたとき「2 人分だけ新しい」中途半端な状態になる。
        // いったん仮の配列に読み、全部正しかったときだけ本物に書く (トランザクションの考え方)。
        std::array<PlayerId, kMaxPlayers>    ids = {};
        std::array<PlayerState, kMaxPlayers> states;
        for (int i = 0; i < count; ++i)
        {
            ids[i] = r.U8();
            if (!r.Ok() || !IsValidId(ids[i]) || !ReadState(r, states[i]))
            {
                return;
            }
        }

        hasWorldSeq_ = true;
        lastWorldSeq_ = seq;
        UpdateClockOffset(static_cast<double>(hostTime));

        for (int i = 0; i < count; ++i)
        {
            const PlayerId id = ids[i];
            if (id == localId_)
            {
                continue; // 自分の状態 (ホストは送ってこないはずだが、念のため)
            }
            Peer& peer = peers_[id];
            if (!peer.active)
            {
                continue; // 参加者一覧がまだ届いていない
                // (状態は確実ではない通信、一覧は確実な通信なので、状態が先に届くことがある)
            }
            peer.hasState = true;
            peer.latestState = states[i];
            // ホストの時刻を付けて貯める。表示するときも RenderTimeMs() でホストの時計に換算した時刻で探す。
            peer.snapshots.Push(static_cast<double>(hostTime), states[i]);
        }
    }

    // ホストの時計と自分の時計のずれを推定する。
    //   sample = 自分の今の時刻 − パケットに書かれたホストの時刻
    //          = (時計のずれ) + (パケットが届くまでにかかった時間)
    //   届くまでの時間は毎回ばらつくが、一番早く届いたものが「本当のずれ」に一番近い。
    void NetSession::UpdateClockOffset(double hostTimeMs)
    {
        // offset = 自分の時計 - ホストの時計 (+ 通信にかかった時間)
        // 一番早く届いたもの (遅延が最小のもの) に合わせ、ゆっくり上にも追従する
        const double sample = ElapsedMs() - hostTimeMs;
        if (!hasClockOffset_ || sample < clockOffsetMs_)
        {
            // より小さい値 (より早く届いた) が来たら、すぐそれに合わせる。
            clockOffsetMs_ = sample;
            hasClockOffset_ = true;
        }
        else
        {
            // 大きい値のときは、差の 1% だけ近づける。
            //   一瞬の遅れ (回線が混んだ) にはほとんど反応せず、
            //   ずっと続く変化 (PC の時計の進み方の違い、回線の変化) にはゆっくりついていく。
            clockOffsetMs_ += (sample - clockOffsetMs_) * 0.01;
        }
    }

    // ホストから届いた参加者一覧を反映し、前との違いをイベントにする。
    void NetSession::ApplyRoster(const std::vector<std::uint8_t>& body)
    {
        ByteReader r(body.data(), static_cast<int>(body.size()));
        r.U8(); // 種類
        const bool          inMatch = (r.U8() != 0);
        const std::uint16_t matchNumber = r.U16();
        const std::uint8_t  count = r.U8();
        if (!r.Ok() || count > kMaxPlayers)
        {
            return;
        }

        // HandleWorldState と同じく、まず全部を仮の配列に読む。
        // present[id] = その ID が一覧にいるか、teams[id] = その ID の陣営。
        std::array<bool, kMaxPlayers> present = {};
        std::array<Team, kMaxPlayers> teams = {};
        for (int i = 0; i < count; ++i)
        {
            const std::uint8_t id = r.U8();
            const std::uint8_t team = r.U8();
            // id と team は外から来た値。範囲を確かめずに present[id] と書くと、配列の外に書き込む。
            if (!r.Ok() || !IsValidId(id) || !IsValidTeam(team))
            {
                return;
            }
            present[id] = true;
            teams[id] = static_cast<Team>(team); // 範囲を確かめた後なので、enum に戻してよい
        }

        if (!present[localId_])
        {
            // ホスト側では自分がもう抜けた扱いになっている
            //   (例: 回線が 3 秒以上止まって、ホストにタイムアウトで外された後に、回線が戻った)
            Disconnect(DisconnectReason::Timeout);
            return;
        }

        // 前の一覧 (peers_ の active) と新しい一覧 (present) を 1 人ずつ比べて、違いをイベントにする。
        for (PlayerId id = 0; id < kMaxPlayers; ++id)
        {
            Peer& peer = peers_[id];
            if (present[id] && !peer.active)
            {
                // 新しくいる → 参加した
                peer = Peer();
                peer.active = true;
                peer.team = teams[id];
                PushEvent(NetEventType::PlayerJoined, id);
            }
            else if (!present[id] && peer.active)
            {
                // いなくなった → 抜けた
                // 参加者側では、抜けた理由 (自分で抜けた / タイムアウト) は分からないので reason は None。
                peer = Peer();
                PushEvent(NetEventType::PlayerLeft, id);
            }
            else if (present[id] && peer.team != teams[id])
            {
                // 陣営が変わった
                peer.team = teams[id];
                PushEvent(NetEventType::TeamChanged, id);
            }
        }

        // 試合の状態の変化をイベントにする。
        if (inMatch)
        {
            // ロビー → 試合中、または「試合中のまま試合番号が変わった」(終了と開始の一覧を両方見逃した) とき。
            if (!inMatch_ || matchNumber != matchNumber_)
            {
                if (inMatch_)
                {
                    PushEvent(NetEventType::MatchEnded, kInvalidPlayerId); // 前の試合を終わらせてから
                }
                PushEvent(NetEventType::MatchStarted, kInvalidPlayerId);  // 新しい試合を始める
            }
            state_ = SessionState::InMatch;
        }
        else
        {
            if (inMatch_)
            {
                PushEvent(NetEventType::MatchEnded, kInvalidPlayerId);
            }
            state_ = SessionState::Lobby;
        }
        inMatch_ = inMatch;
        matchNumber_ = matchNumber;
        // 補足: 一覧は確実な通信で「順番どおり・全部」届くので、上の「両方見逃した」は普通は起きない。
        //       それでもゲーム側が必ず End → Start の順にイベントを受け取れるよう、念のため書いてある。
    }

    //==========================================================
    // 確実に届ける通信 (ACK と再送)
    //==========================================================
    bool NetSession::EnqueueReliable(ReliableChannel& channel, const std::vector<std::uint8_t>& body)
    {
        if (channel.pending.size() >= kMaxPendingReliable ||
            body.size() > static_cast<std::size_t>(kMaxReliableBodySize))
        {
            return false;
        }
        ReliableChannel::Pending pending;
        pending.seq = channel.nextSendSeq++;
        pending.body = body;
        channel.pending.push_back(std::move(pending));
        return true;
    }

    void NetSession::FlushReliable(ReliableChannel& channel, const NetAddress& to, std::uint32_t token)
    {
        // 送るものを1つのパケットにまとめる (小さなメッセージを1個ずつ送るより消えにくく、無駄が少ない)
        std::vector<ReliableChannel::Pending*> batch;
        int batchBytes = 0;
        int packets = 0;

        auto sendBatch = [&]()
            {
                if (batch.empty())
                {
                    return;
                }
                PacketBuffer buffer;
                ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
                WriteHeader(w, MsgType::Reliable, token);
                w.U8(localId_);
                w.U8(static_cast<std::uint8_t>(batch.size()));
                for (const ReliableChannel::Pending* p : batch)
                {
                    w.U16(p->seq);
                    w.U16(static_cast<std::uint16_t>(p->body.size()));
                    w.Bytes(p->body.data(), static_cast<int>(p->body.size()));
                }
                if (w.Ok())
                {
                    SendPacket(to, w.Data(), w.Size());
                }
                for (ReliableChannel::Pending* p : batch)
                {
                    p->sent = true;
                    p->lastSent = now_;
                }
                batch.clear();
                batchBytes = 0;
                ++packets;
            };

        if (!channel.pending.empty())
        {
            const std::uint16_t base = channel.pending.front().seq;
            for (ReliableChannel::Pending& pending : channel.pending)
            {
                // 受け手が預かれる範囲を超えて送っても捨てられるだけなので、窓の中だけ送る
                if (static_cast<std::uint16_t>(pending.seq - base) >= kSendWindow)
                {
                    break;
                }
                if (pending.acked)
                {
                    continue;
                }
                if (pending.sent && now_ - pending.lastSent < config_.reliableResendInterval)
                {
                    continue; // 返事を待っている
                }

                const int entryBytes = 4 + static_cast<int>(pending.body.size()); // seq(2) + len(2) + 本体
                if (kHeaderSize + 2 + batchBytes + entryBytes > detail::kMaxPacketSize || batch.size() >= 255)
                {
                    sendBatch();
                    if (packets >= kMaxReliablePacketsPerFlush)
                    {
                        break;
                    }
                }
                batch.push_back(&pending);
                batchBytes += entryBytes;
            }
            sendBatch();
        }

        // 受け取ったものがあれば、どこまで受け取ったかを返す
        if (channel.ackPending)
        {
            // nextExpected より先に届いて預かっている番号も、ビットで知らせる (選択的 ACK)
            std::uint32_t mask = 0;
            for (const auto& kv : channel.outOfOrder)
            {
                const int distance = static_cast<std::uint16_t>(kv.first - channel.nextExpected);
                if (distance >= 1 && distance <= kAckMaskBits)
                {
                    mask |= (1u << (distance - 1));
                }
            }

            PacketBuffer buffer;
            ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
            WriteHeader(w, MsgType::Ack, token);
            w.U8(localId_);
            w.U16(channel.nextExpected);
            w.U32(mask);
            if (w.Ok())
            {
                SendPacket(to, w.Data(), w.Size());
            }
            channel.ackPending = false;
        }
    }

    void NetSession::ReceiveReliable(ReliableChannel& channel, PlayerId senderId, const std::uint8_t* data, int size)
    {
        ByteReader r(data, size);
        const std::uint8_t count = r.U8();
        if (!r.Ok())
        {
            return;
        }

        // 重複でも ACK は返す (前の ACK が消えたのかもしれない)
        channel.ackPending = true;

        for (int i = 0; i < count; ++i)
        {
            const std::uint16_t seq = r.U16();
            const std::uint16_t len = r.U16();
            if (!r.Ok() || len > kMaxReliableBodySize || len > r.Remaining())
            {
                return; // 壊れている。ここから先は読まない
            }
            std::vector<std::uint8_t> body(len);
            r.Bytes(body.data(), len);
            ReceiveReliableEntry(channel, senderId, seq, std::move(body));
        }
    }

    void NetSession::ReceiveReliableEntry(ReliableChannel& channel, PlayerId senderId, std::uint16_t seq,
        std::vector<std::uint8_t> body)
    {
        if (seq == channel.nextExpected)
        {
            ++channel.nextExpected;
            ProcessReliableBody(senderId, body);

            // 先に届いて預かっていた続きを、順番に処理する
            for (;;)
            {
                const auto it = channel.outOfOrder.find(channel.nextExpected);
                if (it == channel.outOfOrder.end())
                {
                    break;
                }
                const std::vector<std::uint8_t> next = std::move(it->second);
                channel.outOfOrder.erase(it);
                ++channel.nextExpected;
                ProcessReliableBody(senderId, next);
            }
        }
        else if (detail::SeqNewer16(seq, channel.nextExpected))
        {
            // 間が抜けている → 届くまで預かる
            const int distance = static_cast<std::uint16_t>(seq - channel.nextExpected);
            if (distance < kSendWindow && channel.outOfOrder.size() < kMaxOutOfOrder)
            {
                channel.outOfOrder.emplace(seq, std::move(body));
            }
        }
        // それ以外は処理済みの重複なので捨てる
    }

    void NetSession::HandleAck(ReliableChannel& channel, std::uint16_t nextExpected, std::uint32_t receivedMask)
    {
        for (ReliableChannel::Pending& pending : channel.pending)
        {
            if (detail::SeqNewer16(nextExpected, pending.seq))
            {
                pending.acked = true; // nextExpected より前は全部届いている
                continue;
            }
            const int distance = static_cast<std::uint16_t>(pending.seq - nextExpected);
            if (distance >= 1 && distance <= kAckMaskBits && ((receivedMask >> (distance - 1)) & 1u) != 0)
            {
                pending.acked = true; // 先の番号で、届いたと知らされたもの
            }
        }
        while (!channel.pending.empty() && channel.pending.front().acked)
        {
            channel.pending.pop_front();
        }
    }

    void NetSession::ProcessReliableBody(PlayerId senderId, const std::vector<std::uint8_t>& body)
    {
        if (body.empty())
        {
            return;
        }
        switch (static_cast<ReliableKind>(body[0]))
        {
        case ReliableKind::Roster:
            if (!isHost_)
            {
                ApplyRoster(body);
            }
            break;

        case ReliableKind::GameEvent:
            ProcessGameEvent(senderId, body);
            break;

        default:
            break;
        }
    }

    void NetSession::ProcessGameEvent(PlayerId senderId, const std::vector<std::uint8_t>& body)
    {
        ByteReader r(body.data(), static_cast<int>(body.size()));
        r.U8(); // 種類
        const PlayerId      origin = r.U8();
        const std::uint8_t  target = r.U8();
        const std::uint16_t type = r.U16();
        const std::uint16_t len = r.U16();
        if (!r.Ok() || !IsValidId(origin) || target > static_cast<std::uint8_t>(EventTarget::HostOnly) ||
            len > kMaxEventPayloadSize || len != r.Remaining())
        {
            return;
        }
        std::vector<std::uint8_t> payload(len);
        r.Bytes(payload.data(), len);

        if (isHost_)
        {
            if (origin != senderId)
            {
                return; // 他人のふりは受け付けない
            }
            // 全員宛てなら、送り主以外の参加者に中継する
            if (static_cast<EventTarget>(target) == EventTarget::All)
            {
                for (PlayerId id = 1; id < kMaxPlayers; ++id)
                {
                    if (id != origin && peers_[id].active)
                    {
                        EnqueueReliable(peers_[id].channel, body);
                    }
                }
            }
        }
        else if (origin == localId_)
        {
            return;
        }

        PushGameEvent(origin, type, std::move(payload));
    }

    bool NetSession::SendGameEvent(std::uint16_t type, const void* data, int size, EventTarget target)
    {
        if (state_ != SessionState::Lobby && state_ != SessionState::InMatch)
        {
            return false;
        }
        if (size < 0 || size > kMaxEventPayloadSize || (size > 0 && data == nullptr))
        {
            return false;
        }
        if (isHost_ && target == EventTarget::HostOnly)
        {
            return false;
        }

        std::array<std::uint8_t, kMaxReliableBodySize> buffer;
        ByteWriter w(buffer.data(), static_cast<int>(buffer.size()));
        w.U8(static_cast<std::uint8_t>(ReliableKind::GameEvent));
        w.U8(localId_);
        w.U8(static_cast<std::uint8_t>(target));
        w.U16(type);
        w.U16(static_cast<std::uint16_t>(size));
        w.Bytes(data, size);
        if (!w.Ok())
        {
            return false;
        }
        const std::vector<std::uint8_t> body(w.Data(), w.Data() + w.Size());

        if (!isHost_)
        {
            return EnqueueReliable(hostChannel_, body);
        }

        bool ok = true;
        for (PlayerId id = 1; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active)
            {
                ok = EnqueueReliable(peers_[id].channel, body) && ok;
            }
        }
        return ok;
    }

    //==========================================================
    // その他
    //==========================================================
    void NetSession::SetLocalState(const PlayerState& state)
    {
        localState_ = state;
        if (localState_.customSize > kMaxCustomStateSize)
        {
            localState_.customSize = kMaxCustomStateSize;
        }
    }

    bool NetSession::PollEvent(NetEvent& out)
    {
        if (events_.empty())
        {
            return false;
        }
        out = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    void NetSession::PushEvent(NetEventType type, PlayerId player, DisconnectReason reason)
    {
        if (events_.size() >= kMaxEvents)
        {
            events_.pop_front(); // 取り出されないまま溜まり続けるのを防ぐ
        }
        NetEvent e;
        e.type = type;
        e.player = player;
        e.reason = reason;
        events_.push_back(std::move(e));
    }

    void NetSession::PushGameEvent(PlayerId sender, std::uint16_t type, std::vector<std::uint8_t> payload)
    {
        if (events_.size() >= kMaxEvents)
        {
            events_.pop_front();
        }
        NetEvent e;
        e.type = NetEventType::GameEvent;
        e.player = sender;
        e.gameEventType = type;
        e.payload = std::move(payload);
        events_.push_back(std::move(e));
    }

    void NetSession::Disconnect(DisconnectReason reason)
    {
        if (state_ == SessionState::Idle || state_ == SessionState::Disconnected)
        {
            return;
        }
        state_ = SessionState::Disconnected;
        PushEvent(NetEventType::Disconnected, localId_, reason);
    }

    bool NetSession::SendPacket(const NetAddress& to, const std::uint8_t* data, int size)
    {
        return transport_.SendTo(to, data, size);
    }

    void NetSession::SendLeave(const NetAddress& to, std::uint32_t token)
    {
        PacketBuffer buffer;
        ByteWriter   w(buffer.data(), static_cast<int>(buffer.size()));
        WriteHeader(w, MsgType::Leave, token);
        w.U8(localId_);
        if (!w.Ok())
        {
            return;
        }
        for (int i = 0; i < kLeaveRepeatCount; ++i)
        {
            SendPacket(to, w.Data(), w.Size());
        }
    }

    int NetSession::PlayerCount() const
    {
        int count = 0;
        for (const Peer& peer : peers_)
        {
            if (peer.active) { ++count; }
        }
        return count;
    }

    void NetSession::FillPlayerInfo(PlayerId id, PlayerInfo& out, double renderTimeMs) const
    {
        const Peer& peer = peers_[id];
        out = PlayerInfo();
        out.id = id;
        out.team = peer.team;
        out.isLocal = (id == localId_);
        out.isHost = (id == kHostPlayerId);
        if (out.isLocal)
        {
            out.state = localState_;
            out.hasState = true;
        }
        else if (peer.hasState)
        {
            out.hasState = peer.snapshots.Sample(renderTimeMs, out.state);
        }
    }

    int NetSession::GetPlayers(std::array<PlayerInfo, kMaxPlayers>& out) const
    {
        if (state_ != SessionState::Lobby && state_ != SessionState::InMatch)
        {
            return 0;
        }
        const double renderTime = RenderTimeMs();
        int count = 0;
        for (PlayerId id = 0; id < kMaxPlayers; ++id)
        {
            if (peers_[id].active)
            {
                FillPlayerInfo(id, out[count++], renderTime);
            }
        }
        return count;
    }

    bool NetSession::GetPlayer(PlayerId id, PlayerInfo& out) const
    {
        if (!IsValidId(id) || !peers_[id].active ||
            (state_ != SessionState::Lobby && state_ != SessionState::InMatch))
        {
            return false;
        }
        FillPlayerInfo(id, out, RenderTimeMs());
        return true;
    }
}