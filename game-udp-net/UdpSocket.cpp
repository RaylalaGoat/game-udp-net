#include "UdpSocket.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

// =========================================================
// UqpSocket.cpp
//
// 制作者:h		日付：
// 
// WinsockContext(WSACleanup)
// Open     : /////
// SendTo   : ReceiveFrom を毎フレーム呼ぶ
// Close    : closesocket
// WinsockContextの破棄(WSACleanup)
// =========================================================

// このファイルだけで使える 衝突しない
namespace
{
    // unintptr_t で持っているソケット番号を Winsock SOCKET型に戻す
    SOCKET ToSocket(std::uintptr_t s) { return static_cast<SOCKET>(s); }
}

//==============================================================
// WinsockContext
//==============================================================
WinsockContext::WinsockContext()
{
    // WSAStarupの結果(使える Winsockバージョンなど)
    WSADATA data;

    // MAKEWORD(2, 2) : バージョン2, 2を使う
    // 成功すると 0 を返す
    ok_ = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
}

WinsockContext::~WinsockContext()
{
    // 成功したときだけ終了処理 WSAStartup と WSASCleanup は必ず同じ回数呼ぶ
    if (ok_)
    {
        WSACleanup();
    }
}

//==============================================================
// UdpSocket
//==============================================================
UdpSocket::~UdpSocket()
{
    // デストラクタで閉じる
    // main で return しても、途中でエラー担っても、必ずここを通る
    Close();
}

bool UdpSocket::IsOpen() const
{
    // INVALID_ID_SOCKET (全ビット1) でなければ開いている
    return ToSocket(sock_) != INVALID_SOCKET;
}

//--------------------------------------------------------------
// Open: ソケットを作って、使える状態にする (4 段階)
//--------------------------------------------------------------

bool UdpSocket::Open(std::uint16_t localPort)
{
    //既に開いている場合閉じてから開き直す
    Close();

    // =(1)= ソケットを作る
    // AF_INET      : IPv4を使う
    // SOCK_DGRAM   : 1個ずつ区切られたパケットで送る方式(UDP)
    // IPPROTO_UDP  : UDPを使う
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
    {
        // 失敗の理由は戻り値ではなく WSAGetLastError で取り出す
        // すぐ取り出さないと次の Winsock で上書きされる
        lastError_ = WSAGetLastError();
        return false;
    }

    // =(2)= 自分のポートで受信する (0 なら OS が空いている番号を選ぶ)
    // sockaddr_in = IPv4 の住所を入れる構造体 全部0 にしてから使う
    sockaddr_in addr = {};
    addr.sin_family      = AF_INET;             // IPv4の住所
    addr.sin_port        = htons(localPort);    // ポートをビッグエンディアンに並べ替える
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // INADDR_ANY この 全ての ネットワークの口で受け取る
                                                // Wi-Fi 有線 LAnケーブル直結でも動く

    // bind ソケットに住所 (IP とポート)を結びつける これでポートに届いたパケットを受け取れる 
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
    {
        lastError_ = WSAGetLastError();
        closesocket(s); // =(1)= で作ったソケットを閉じてから失敗を返す
        return false;
    }

    // =(3)=ノンブロッキング化 (recvfrom がゲームループを止めないようにする)
    //  普通の recvfrom は、パケットが届くまで関数から戻ってこない。
    //  ゲームでそれをすると、相手が何も送ってこない間は画面が固まる。
    //  ノンブロッキングにすると、何も届いていないときは WSAEWOULDBLOCK エラーですぐ戻る。
    //  FIONBIO 設定 1 で有効。
    //  u_long は Winsock が使う型 (unsigned long)。ioctlsocket がこの型のポインタを要求する。
    u_long nonBlocking = 1;
    if (ioctlsocket(s, FIONBIO, &nonBlocking) == SOCKET_ERROR)
    {
        lastError_ = WSAGetLastError();
        closesocket(s);
        return false;
    }

    // 相手がまだ起動していないポートに送ると、Windows では以後の recvfrom が
    // WSAECONNRESET を返す。その報告を無効にする。
    // 失敗しても ReceiveFrom 側で WSAECONNRESET を読み飛ばすので、戻り値は見ない。

    //   誰も待っていないポートに UDP を送ると、相手の PC から「そこには誰もいない」という
    //   (エラー通知用の小さなパケット) が返ってくる。
    //   Windows はそれを受け取ると、次の recvfrom を WSAECONNRESET (10054) で失敗させる。
    //   UDP は「つながり」がないのに「接続がリセットされた」と言ってくる
    //   例: 参加者が先に起動して、まだ起動していないホストに参加申し込みを送ったとき。
    //   エラーだと思って切断してしまう。
    BOOL  reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset),
             nullptr, 0, &bytes, nullptr, nullptr);

    // 全部成功してから、メンバに保存する。
    // 途中で失敗したときに「作りかけのソケット」がメンバに残らないようにするため。
    sock_ = static_cast<std::uintptr_t>(s);
    return true;
}

void UdpSocket::Close()
{
    if (IsOpen())
    {
        closesocket(ToSocket(sock_));   // winsock では close ではなくclosesocket
        sock_ = static_cast<std::uintptr_t>(INVALID_SOCKET); // 「開いていない」に戻す 二重の閉じる事故を防ぐ
    }
    hasPeer_ = false;
}

//--------------------------------------------------------------
// ParseIPv4: 192.168.0.10 → 数値
//--------------------------------------------------------------
bool UdpSocket::ParseIPv4(const char* text, std::uint32_t& ip)
{
    // nullptr を inet_oton に渡すとクラッシュするので、先に弾く
    if (text == nullptr)
    {
        return false;
    }

    // inet_pton = presentation (人が読む形) to network (数値)。
    // 成功で 1、形式が正しくないと 0、エラーで -1 を返す
    in_addr a = {};
    if (inet_pton(AF_INET, text, &a) != 1)
    {
        return false;
    }
    ip = a.s_addr; // ネットワークバイトオーダーのまま返す
    return true;
}

//--------------------------------------------------------------
// 1対1用の関数 (SetPeer / Send / Receive)
//--------------------------------------------------------------

bool UdpSocket::SetPeer(const char* ipv4, std::uint16_t port)
{
    std::uint32_t ip = 0;
    if (!ParseIPv4(ipv4, ip))
    {
        lastError_ = WSAEINVAL;
        return false;
    }
    peerAddr_ = ip;
    peerPort_ = htons(port); // 覚えるときにネットワークバイトオーダーにする 送るたびに変換しなくていい
    hasPeer_  = true;
    return true;
}

bool UdpSocket::Send(const void* data, int size)
{
    if (!hasPeer_)
    {
        return false;
    }
    // SendTo に任せる
    return SendTo(peerAddr_, peerPort_, data, size);
}

//--------------------------------------------------------------
// SendTo: 宛先を指定して1個送る
//--------------------------------------------------------------

bool UdpSocket::SendTo(std::uint32_t ip, std::uint16_t port, const void* data, int size)
{
    if (!IsOpen())
    {
        return false;
    }

    // 宛先の住所を作る。ip と port は既にネットワークバイトオーダーなので、そのまま入れる。
    sockaddr_in to = {};
    to.sin_family      = AF_INET;
    to.sin_port        = port;
    to.sin_addr.s_addr = ip;

    // sendto: 1個のパケットを送る。
    // 第2引数が const char* なのは、Winsock が C 言語時代の API だから (void* ではない)。
    // void* → const char* は static_cast で変換できる (reinterpret_cast は不要)。
    // 第4引数 0 = 特別な指定なし。
    // UDP では「全部送れた」か「失敗」のどちらか
    const int sent = sendto(ToSocket(sock_), static_cast<const char*>(data), size, 0,
                            reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    if (sent == SOCKET_ERROR)
    {
        lastError_ = WSAGetLastError();
        return false;
    }
    // true は「OS に渡せた」だけ。相手に届いたかは分からない
    return true;
}

int UdpSocket::Receive(void* buffer, int bufferSize)
{
    for (;;) // 終わりの条件を書かない無限ループ。中の return か continue で出入りする。
    {
        std::uint32_t ip   = 0;
        std::uint16_t port = 0;
        const int n = ReceiveFrom(buffer, bufferSize, ip, port);
        if (n <= 0)
        {
            return n; // 0 = 何も届いていない / -1 = エラー。どちらもそのまま返す
        }

        // 相手以外から届いたものは捨てる
        // (UDP のソケットは、誰からでも受け取ってしまう。知らない相手のパケットで動きが乱れないようにする)
        if (hasPeer_ && (ip != peerAddr_ || port != peerPort_))
        {
            continue; // 次のパケットを見る
        }
        return n;
    }
}

//--------------------------------------------------------------
// ReceiveFrom: 誰からでも1個受け取る (複数人版が使う)
//--------------------------------------------------------------

int UdpSocket::ReceiveFrom(void* buffer, int bufferSize, std::uint32_t& ip, std::uint16_t& port)
{
    if (!IsOpen())
    {
        return -1;
    }

    for (;;)
    {
        sockaddr_in from = {};      // 送り主の住所
        int fromLen = sizeof(from); // 入力: from の大きさ / 出力: 書き込まれた大きさ

        // recvfrom: 届いているパケットを 1 個取り出す。
        // ノンブロッキングなので、何も届いていなければ待たずに SOCKET_ERROR で戻る。
        // 1回の recvfrom で取り出せるのは必ず 1 個のパケット
        const int n = recvfrom(ToSocket(sock_), static_cast<char*>(buffer), bufferSize, 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);

        // 受信できた。送り主の IP とポートを返す (ネットワークバイトオーダーのまま)。
        if (n != SOCKET_ERROR)
        {
            ip   = from.sin_addr.s_addr;
            port = from.sin_port;
            return n;
        }

        const int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK)
        {
            return 0; // 何も届いていない
        }
        if (err == WSAECONNRESET || err == WSAEMSGSIZE)
        {
            continue; // その1個は捨てて次を見る
        }

        // エラー。番号を覚えて -1 を返す → NetSession は TransportError で切断する。
        lastError_ = err;
        return -1;
    }
}

//--------------------------------------------------------------
// LocalPort: OS が選んだポート番号を調べる
//--------------------------------------------------------------

std::uint16_t UdpSocket::LocalPort() const
{
    if (!IsOpen())
    {
        return 0;
    }

    // getsockname: ソケットに結びついている自分の住所を調べる。
    // Open(0) で OS に選ばせたときは、これで実際の番号が分かる。
    sockaddr_in addr = {};
    int len = sizeof(addr);
    if (getsockname(ToSocket(sock_), reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR)
    {
        return 0;
    }
    return ntohs(addr.sin_port);
}
