#pragma once
#include "ITransport.h"
#include "NetTypes.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <type_traits>
#include <vector>

// =========================================================
// NetSession.h
//
// 制作者:h		日付：
// =========================================================

// ==============================================================
// 通信モジュールの窓口 (ゲーム側が使うのはこのファイルだけ)
//
// 使い方:
//   開始時   : Host() または Join(ホストの住所)
//   毎フレーム: Update() → PollEvent() で出来事を処理
//              → SetLocalState(自分の状態) → GetPlayers() で全員を描画
//   出来事   : SendGameEvent(種類, データ) で確実に届ける
//   ホストのみ: SetTeam() / StartMatch() / EndMatch()
// ==============================================================
namespace net
{
    // ==========================================================
    // 1: 上限の定数
    // ==========================================================
    // 状態 (毎秒 30 回送る) に載せる自由なデータの上限。
    // 4 人分を 1 つの WorldState パケット (1200 バイトまで) に入れるので、大きくしすぎると入らなくなる
    constexpr int kMaxCustomStateSize  = 64;  // 状態に載せる自由なデータの最大バイト数

    // 1 つのイベントの中身の上限。確実に届けるメッセージは 1 パケットに収める作りなので、上限を決めている。
    // (写真のような大きなデータを送るには、分割して送る仕組みを足す必要がある)

    constexpr int kMaxEventPayloadSize = 256; // 1つのイベントの最大バイト数

    // ==========================================================
    // 2: 位置と向き
    // ==========================================================
    // DirectXMath の XMFLOAT3 などを使わず float の配列にしているのは、
    // 通信モジュールを DirectX に依存させないため
    // ゲーム側で XMFLOAT3  position[3] を変換して使う。
    struct Transform
    {
        float position[3] = { 0.0f, 0.0f, 0.0f };

        // 初期値の (0, 0, 0, 1) は「回転なし」を表す。
        // (0, 0, 0, 0) にすると回転として意味を持たない値になり、補間の計算で 0 除算などの事故になる
        float rotation[4] = { 0.0f, 0.0f, 0.0f, 1.0f }; // クォータニオン (x, y, z, w)
    };

    // ==========================================================
    // 3: プレイヤーの状態
    // ==========================================================
    // 毎フレーム同期するプレイヤーの状態
    // 位置と向きは補間される。それ以外 (アニメーション・HP など) は custom に入れる

    // 【なぜ custom を「ただのバイト列」にしているか】
    //  通信モジュールは、ゲームに何の値があるかを知らない
    //  ゲームごとに変わる部分はバイト列として預かり、中身の意味はゲーム側に任せる
    //  こうすると、ゲームの仕様が変わっても通信モジュールを直さなくてよい
    struct PlayerState
    {
        Transform     transform;
        std::uint8_t  customSize = 0; // custom の何バイト目までが有効か (0〜64)。パケットにもこの長さだけ書く
        std::array<std::uint8_t, kMaxCustomStateSize> custom = {};

        // ゲームが決めた構造体を custom に入れる
        // 例: struct AnimState { std::uint16_t anim; std::uint8_t hp; };

        // template <class T> : どんな型の構造体でも受け取れる関数を、型ごとに自動で作る仕組み
        // テンプレートは「使われた所で中身が作られる」ので、中身をヘッダに書く必要がある
        template <class T>
        void SetCustom(const T& value)
        {
            // static_assert : コンパイル時のチェック。条件が false ならコンパイルエラーにする
            // 実行してから気づくのではなく、ビルドした瞬間に間違いが分かる

            // std::is_trivially_copyable_v<T> : 「memcpy でコピーしても正しくコピーできる型か」
            //   ・ポインタを持つ型   → アドレスは相手の PC では意味がない
            //   ・std::string や std::vector → 中身は別の場所 (ヒープ) にあり、ポインタしか送られない
            //   ・仮想関数を持つ型   → 仮想関数表へのポインタが入っている
            //   こういう型を memcpy で送ると、相手の PC で壊れた値になる。それをコンパイル時に止める
            static_assert(std::is_trivially_copyable_v<T>, "SetCustom: ポインタや仮想関数を持たない単純な構造体だけ使える");
            static_assert(sizeof(T) <= kMaxCustomStateSize, "SetCustom: kMaxCustomStateSize を超えている");
            std::memcpy(custom.data(), &value, sizeof(T));
            customSize = static_cast<std::uint8_t>(sizeof(T));
        }

        // custom を構造体として取り出す。大きさが合わなければ false

        //   大きさが違う = 送った側と型が違う、と判断して読まない (違う型として読むと値が壊れるため)
        //   ただし「大きさが同じで中身が違う型」は見分けられない
        template <class T>
        bool GetCustom(T& out) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "GetCustom: ポインタや仮想関数を持たない単純な構造体だけ使える");
            if (customSize != sizeof(T))
            {
                return false; // まだ何も入っていない (0) ときもここで false になる
            }
            std::memcpy(&out, custom.data(), sizeof(T));
            return true;
        }
    };

    // ==========================================================
    // 4: GetPlayers で受け取る 1 人分の情報
    // ==========================================================

    struct PlayerInfo
    {
        PlayerId    id       = kInvalidPlayerId;
        Team        team     = Team::None;
        bool        isLocal  = false; // 自分自身
        bool        isHost   = false;
        bool        hasState = false; // まだ状態が一度も届いていなければ false
                                      // false のときの stateは初期値なので 描画に使わない
        PlayerState state;            // 他のプレイヤーは補間済み(0.1 秒前の位置をなめらかにしたもの)
    };

    // ==========================================================
    // 5: イベント
    // ==========================================================
    // PollEvent() で受け取る出来事の種類
    // 自分の PC の中だけで使う値なので、番号は書いていない。
    enum class NetEventType : std::uint8_t
    {
        Connected,    // 参加できた (player = 自分の ID)
        Disconnected, // 切断された (reason を見る)
        PlayerJoined, // 誰かが参加した
        PlayerLeft,   // 誰かが抜けた
        TeamChanged,  // 陣営が変わった
        MatchStarted, // 試合開始
        MatchEnded,   // 試合終了 (ロビーに戻った)
        GameEvent,    // SendGameEvent で送られた出来事
    };

    // 1 つの出来事。種類によって使うメンバが違う (使わないメンバは初期値のまま)。
    struct NetEvent
    {
        NetEventType     type          = NetEventType::GameEvent;
        PlayerId         player        = kInvalidPlayerId;          // 関係するプレイヤー / GameEvent の送り主
        DisconnectReason reason        = DisconnectReason::None;    // Disconnected / PlayerLeft のときだけ意味がある
        std::uint16_t    gameEventType = 0;                         // GameEvent のときだけ: ゲームが決めた種類の番号
        std::vector<std::uint8_t> payload;                          // GameEvent のときだけ: 中身のバイト列

        // payload を vector (大きさが変わる配列) にしているのは、イベントごとに大きさが違うため。
        // vector はヒープを確保するが、イベントは毎フレーム大量に来るものではないので問題にならない。
        // (毎フレーム届く「状態」の方は、確保が起きない std::array で持っている)

        // payload を構造体として取り出す。大きさが合わなければ false
        // GetCustom と同じ考え方。送った側と同じ型で読む
        template <class T>
        bool Read(T& out) const
        {
            static_assert(std::is_trivially_copyable_v<T>, "Read: ポインタや仮想関数を持たない単純な構造体だけ使える");
            if (payload.size() != sizeof(T))
            {
                return false;
            }
            std::memcpy(&out, payload.data(), sizeof(T));
            return true;
        }
    };

    // ==========================================================
    // : 宛先と設定
    // ==========================================================
    // イベントを誰に届けるか
    enum class EventTarget : std::uint8_t
    {
        All,      // 自分以外の全員
        HostOnly, // ホストだけ (参加者からホストへの依頼に使う。ホストは使えない)
    };

    struct SessionConfig
    {
        double stateSendRate          = 30.0; // 状態を送る回数 (回/秒)
        double timeoutSeconds         = 3.0;  // この時間届かなければ切断
        double interpolationDelay     = 0.1;  // 補間のために遅らせて表示する時間 (秒)
        double joinTimeoutSeconds     = 5.0;  // 参加の返事を待つ時間
        double joinRetryInterval      = 0.25; // 参加の申し込みを送り直す間隔
        double reliableResendInterval = 0.2;  // 確実なメッセージを送り直す間隔
        bool   allowJoinInMatch       = false;// 試合中の途中参加を許すか
    };

    // ==========================================================
    // 7: NetSession (本体)
    // ==========================================================

    class NetSession
    {
    public:
        // 秒を返す関数 (テストで時間を差し替える用)
        // std::function<double()> = 「引数なしで double を返す関数」なら何でも入る入れ物。
        // 普段は何も渡さない (nullptr) → 中で std::chrono::steady_clock を使う時計になる
    
        using Clock = std::function<double()>; // 秒を返す関数 (テストで時間を差し替える用)

        // transport は NetSession より長く生きていること
        // explicit : 引数 1 つで呼べるコンストラクタが、勝手に型変換に使われるのを防ぐ。
        // explicit がないと、NetSession を受け取る関数に transport を渡したとき、
        // こっそり一時的な NetSession が作られてしまう (意図しないオブジェクトが生まれる事故)。
        //
        // デフォルト引数:
        // config と clock は省略できる。NetSession session(transport); だけで使える。
        // const SessionConfig& = SessionConfig() : 一時オブジェクトを参照で受けるが、
        // 中で config_ にコピーするので、関数が終わって一時オブジェクトが消えても問題ない。
        explicit NetSession(ITransport& transport, const SessionConfig& config = SessionConfig(),
                            Clock clock = nullptr);

        // デストラクタは Leave() を呼ぶ → 相手に「抜けます」を送ってから消える
        ~NetSession();

        // コピー禁止。2 つの NetSession が同じ transport・同じ参加者を持つと、二重に送受信して壊れる。
        NetSession(const NetSession&) = delete;
        NetSession& operator=(const NetSession&) = delete;

        // ------------------------------------------------------
        // 開始と終了
        // ------------------------------------------------------
        // 
        // ホストになる。transport は決まったポートで開いておくこと
        // Idle か Disconnected のときだけ成功する。成功するとすぐ Lobby になり、Connected イベントが届く

        bool Host();

        // ホストに参加する。結果は Connected / Disconnected イベントで届く
        bool Join(const NetAddress& hostAddress);

        // 抜ける。何度呼んでもよい。状態は Idle に戻る
        void Leave();

        // ------------------------------------------------------
        // 毎フレーム
        // ------------------------------------------------------
        // 毎フレーム呼ぶ (受信・切断判定・送信・再送)
        // NetSession は自分でスレッドを持たない。ゲームのループから呼ばれたときだけ動く

        void Update();

        // 自分の状態を渡す (毎フレーム)
        // 渡した値はすぐには送られず、次に送る時刻 (1/30 秒ごと) に送られる。
        void SetLocalState(const PlayerState& state);

        // ------------------------------------------------------
        // 確実に届ける出来事
        // ------------------------------------------------------
        // 出来事を確実に届ける
        //   type   : ゲームが決めた種類の番号 (例: 1 = 攻撃、2 = アイテムを拾った)
        //   data / size : 中身。size は 0〜256
        //   失敗 (false) するのは: Lobby / InMatch 以外のとき、大きすぎるとき、
        //                         ホストが HostOnly を指定したとき、送信待ちがいっぱいのとき
        bool SendGameEvent(std::uint16_t type, const void* data, int size,
                           EventTarget target = EventTarget::All);

        // 構造体をそのまま渡せる版
        template <class T>
        bool SendGameEvent(std::uint16_t type, const T& data, EventTarget target = EventTarget::All)
        {
            static_assert(std::is_trivially_copyable_v<T>, "SendGameEvent: ポインタや仮想関数を持たない単純な構造体だけ使える");
            static_assert(sizeof(T) <= kMaxEventPayloadSize, "SendGameEvent: kMaxEventPayloadSize を超えている");
            return SendGameEvent(type, &data, static_cast<int>(sizeof(T)), target);
        }

        // 出来事を1つ取り出す。無ければ false
        bool PollEvent(NetEvent& out);

        // ---- ホストだけが使える ----
        bool SetTeam(PlayerId id, Team team); // ロビー中のみ
        bool StartMatch();                    // Solo 1人 + Squad 1〜3人 のときだけ開始できる
        bool EndMatch();
        // 結果は参加者一覧 (Roster) として全員に配られ、全員に TeamChanged / MatchStarted / MatchEnded イベントが届く。

        // ---- 状態の取得 ----
        SessionState  State() const { return state_; }
        bool          IsHost() const { return isHost_; }
        PlayerId      LocalPlayerId() const { return localId_; }
        std::uint16_t MatchNumber() const { return matchNumber_; } // 試合のたびに 1 増える番号
        int           PlayerCount() const;

        // 全員の情報を out に入れ、人数を返す (メモリ確保なし)
        int  GetPlayers(std::array<PlayerInfo, kMaxPlayers>& out) const;
        bool GetPlayer(PlayerId id, PlayerInfo& out) const;

    private:
        // ======================================================
        // ここから下は NetSession.cpp の中だけで使う
        // ======================================================

        struct Snapshot
        {
            double      time = 0.0; // ホストの時計 (ミリ秒)
            PlayerState state;
        };

        // 受信した状態を時刻つきで貯め、補間に使う
        struct SnapshotBuffer
        {
            static constexpr int kCapacity = 32;
            std::array<Snapshot, kCapacity> items = {};
            int head  = 0; // 一番古い要素の位置
            int count = 0;

            void Clear();
            void Push(double time, const PlayerState& state);   // 新しい状態を足す
            bool Sample(double time, PlayerState& out) const;   // その時刻の状態を補間して求める

            const Snapshot& At(int i) const { return items[(head + i) % kCapacity]; }
        };

        // 確実に届けるための送信待ち・受信待ち
        // 1人の相手につき 1つ持つ。送る側と受ける側の両方の情報が入っている
        struct ReliableChannel
        {
            // 送ったが、まだ返事 (Ack) をもらっていないメッセージ 1 つ
            struct Pending
            {
                std::uint16_t             seq = 0;     // 通し番号
                std::vector<std::uint8_t> body;             // 中身
                double                    lastSent = 0.0;   // 最後に送った時刻 (送り直しの判断に使う)
                bool                      sent = false; // 一度でも送ったか
                bool                      acked = false; // 先の番号だけ届いたと返事があった
                // (まだ前の番号が届いていないので、列からは消せない)
            };

            // ---- 送る側 ----
            std::uint16_t       nextSendSeq = 0;   // 次に付ける通し番号
            std::deque<Pending> pending;            // 返事待ちの列。古い順。返事が来たら前から消す
            // (deque は前から消すのも後ろに足すのも速い)

// ---- 受ける側 ----
            std::uint16_t       nextExpected = 0;   // 次に処理したい番号 (これより前は全部処理済み)
            // 先に届いてしまった番号を預かる表。キー = 通し番号。
            // 欲しい番号が届いたら、続きの番号をここから順番に取り出して処理する。
            std::map<std::uint16_t, std::vector<std::uint8_t>> outOfOrder;
            bool                ackPending = false; // 返事 (Ack) を送る必要があるか
        };

        // 1 人分のプレイヤーの情報 (ホストは全員分、参加者は一覧と状態の分だけ使う)
        struct Peer
        {
            bool           active = false;  // この席が使われているか
            Team           team = Team::None;
            NetAddress     address;               // ホスト側: 参加者の住所
            std::uint32_t  joinNonce = 0;   // ホスト側: 参加申し込みの番号
            std::uint32_t  token = 0;   // ホスト側: この接続の合言葉 (古い接続のパケットを見分ける)
            double         lastReceiveTime = 0.0; // ホスト側: 最後に何か届いた時刻 (タイムアウトの判定)
            bool           hasState = false; // 状態が一度でも届いたか
            std::uint32_t  lastStateSeq = 0;     // 最後に受け取った状態の通し番号 (古い状態を捨てる)
            PlayerState    latestState;             // 最新の状態 (ホストが WorldState で配る)
            SnapshotBuffer snapshots;               // 補間用に貯めた状態
            ReliableChannel channel;              // ホスト側: この参加者との確実な通信
        };

        //------------------------------------------------------
        // 内部の関数 (役割ごとにまとめてある。中身は NetSession.cpp)
        //------------------------------------------------------
        // 共通
        void ResetSession();      // 全部を最初の状態に戻す (Idle)
        void DrainTransport();    // 受信して捨てる (前のセッションの残りを片付ける)
        void ReceivePackets();    // 届いたパケットを全部読んで HandlePacket に渡す
        void HandlePacket(const NetAddress& from, const std::uint8_t* data, int size); // ヘッダを確かめて振り分ける
        void Disconnect(DisconnectReason reason); // 切断して Disconnected にする
        void PushEvent(NetEventType type, PlayerId player, DisconnectReason reason = DisconnectReason::None);
        void PushGameEvent(PlayerId sender, std::uint16_t type, std::vector<std::uint8_t> payload);
        bool SendPacket(const NetAddress& to, const std::uint8_t* data, int size);
        void SendLeave(const NetAddress& to, std::uint32_t token); // 「抜けます」を 3 回送る
        bool IsStateDue();          // 状態を送る時刻になったか
        double ElapsedMs() const;   // 開始からの経過時間 (ミリ秒)
        double RenderTimeMs() const;// 補間で表示する時刻 (ホストの時計で、0.1 秒前)
        void FillPlayerInfo(PlayerId id, PlayerInfo& out, double renderTimeMs) const;

        // 確実な通信
        bool EnqueueReliable(ReliableChannel& channel, const std::vector<std::uint8_t>& body); // 送信待ちに足す
        void FlushReliable(ReliableChannel& channel, const NetAddress& to, std::uint32_t token); // 送る・送り直す・返事する
        void ReceiveReliable(ReliableChannel& channel, PlayerId senderId, const std::uint8_t* data, int size);
        void ReceiveReliableEntry(ReliableChannel& channel, PlayerId senderId, std::uint16_t seq,
            std::vector<std::uint8_t> body); // 1 つのメッセージを順番どおりに処理する
        void HandleAck(ReliableChannel& channel, std::uint16_t nextExpected, std::uint32_t receivedMask);
        void ProcessReliableBody(PlayerId senderId, const std::vector<std::uint8_t>& body); // 種類で振り分ける
        void ProcessGameEvent(PlayerId senderId, const std::vector<std::uint8_t>& body);

        // ホスト
        void HandleHostPacket(const NetAddress& from, std::uint8_t type, std::uint32_t token,
            const std::uint8_t* body, int size);
        void HandleJoinRequest(const NetAddress& from, const std::uint8_t* body, int size);
        void UpdateHost();
        void SendWorldState();       // 全員分の状態を参加者に配る
        void RemovePlayer(PlayerId id, DisconnectReason reason);
        void BroadcastRoster();      // 参加者一覧を全員に確実に配る
        PlayerId FindPeerByAddress(const NetAddress& address) const;
        PlayerId FindFreeSlot() const; // 空いている席を探す。なければ kInvalidPlayerId

        // 参加者
        void HandleClientPacket(const NetAddress& from, std::uint8_t type, std::uint32_t token,
            const std::uint8_t* body, int size);
        void UpdateClient();
        void SendJoinRequest();
        void SendClientState();
        void HandleWorldState(const std::uint8_t* body, int size);
        void ApplyRoster(const std::vector<std::uint8_t>& body); // 一覧を受け取り、前との違いをイベントにする
        void UpdateClockOffset(double hostTimeMs);               // ホストの時計とのずれを推定する

        //------------------------------------------------------
        // メンバ変数
        //------------------------------------------------------
        // 【メンバは「ここに書いた順」に初期化される】
        //   コンストラクタの初期化子リストの順番ではなく、この宣言の順番で初期化される。
        //   例えば clock_ の初期化に config_ を使うなら、config_ が先に宣言されている必要がある。
        //   順番が違うとコンパイラが警告 (GCC/Clang の -Wreorder など) を出すので、宣言と同じ順に書く。

        ITransport& transport_; // 参照で持つ (持ち主ではない)。参照メンバは後から付け替えられない
        SessionConfig config_;    // コピーして持つ (呼んだ側の config が消えても大丈夫)
        Clock         clock_;     // 今の時刻 (秒) を返す関数

        SessionState  state_ = SessionState::Idle;
        bool          isHost_ = false;
        PlayerId      localId_ = kInvalidPlayerId; // 自分の ID (参加が認められるまでは無効)
        std::uint32_t connectionToken_ = 0; // 参加者側: ホストから受け取った合言葉
        bool          inMatch_ = false;
        std::uint16_t matchNumber_ = 0;

        std::array<Peer, kMaxPlayers> peers_; // 席 0〜3。添え字 = PlayerId (peers_[0] はホスト)
        PlayerState            localState_;   // SetLocalState で渡された自分の状態
        std::deque<NetEvent>   events_;       // PollEvent で取り出されるのを待つ出来事の列

        // 時間 (秒)
        double        now_ = 0.0; // この Update の時刻。1 回の Update の中では同じ値を使う
        double        startTime_ = 0.0; // Host() / Join() した時刻
        double        nextStateSendTime_ = 0.0; // 次に状態を送る時刻
        std::uint32_t stateSeq_ = 0;   // 状態パケットの通し番号

        // 参加者側
        NetAddress      hostAddress_;
        std::uint32_t   joinNonce_ = 0;   // 参加申し込みの番号 (乱数)
        double          joinStartTime_ = 0.0; // 申し込みを始めた時刻 (5 秒の判定)
        double          lastJoinSendTime_ = 0.0; // 最後に申し込みを送った時刻 (0.25 秒の判定)
        double          lastHostReceiveTime_ = 0.0; // 最後にホストから何か届いた時刻 (3 秒の判定)
        ReliableChannel hostChannel_;               // ホストとの確実な通信 (参加者は相手がホストだけなので 1 つ)
        bool            hasWorldSeq_ = false;
        std::uint32_t   lastWorldSeq_ = 0;   // 最後に受け取った WorldState の通し番号
        bool            hasClockOffset_ = false;
        double          clockOffsetMs_ = 0.0; // 自分の時計 − ホストの時計 (ミリ秒)。補間の時刻合わせに使う
    };
}
