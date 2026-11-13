#ifndef _UQPSOCKET_H_
#define _UQPSOCKET_H_

#include <cstdint>
// =========================================================
// UqpSocket.h
//
// 制作者:h		日付：
// =========================================================

// このヘッダは winsock2.h を include しない。
// DirectX 側の windows.h と衝突させないため、Winsock は .cpp の中だけで使う。

//==============================================================
// Winsock の初期化 / 終了
// アプリ全体で1つだけ持つ。UdpSocket より先に作り、後に破棄する。
// 
// WASAartup を呼んだら 必ず WSACleanup を呼ばないといけない
//==============================================================
class WinsockContext
{
public:
    WinsockContext();   // WSAStartup を呼ぶ
    ~WinsockContext();  // 成功していたら WSACleanup を呼ぶ

    // コピー禁止。コピーできるとコピーが破棄されたときにも WSACleanup が呼ばれ、
    // まだ使っている途中で Winsock が終了してしまう。
    WinsockContext(const WinsockContext&) = delete;
    WinsockContext& operator=(const WinsockContext&) = delete;

    bool IsOk() const { return ok_; }

private:
    bool ok_ = false;   // WSAStartup に成功したか
};

//==============================================================
// ノンブロッキング UDP ソケット
//   Open        : 自分のポートで受信できるようにする (0 なら OS が空き番号を選ぶ)
//   SetPeer     : 送信先(相手)を決める (1対1用)
//   Send        : SetPeer の相手に送る (届く保証はない)
//   Receive     : SetPeer の相手からの受信を1個読む。無ければすぐ 0 を返す
//   SendTo      : 宛先を指定して送る (複数人用)
//   ReceiveFrom : 誰からでも受信し、送り主を返す (複数人用)
//==============================================================
class UdpSocket
{
public:
    UdpSocket() = default;

    // デストラクタでソケットを閉じる。閉じ忘れが起きない。
    ~UdpSocket();

    // コピー禁止
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // 自分のポートを決めてソケットを開く 0 を渡すと OS が空いている番号を選ぶ
    // ホストは 50000 参加者は 0 で開く (main.cpp)
    // 一台のPCで複数起動しても、参加者同士のポートがぶつからない
    bool Open(std::uint16_t localPort);

    // ソケットを閉じる
    void Close();

    ////////////////////////////////////////
    //====================================//
    // 1対1用 

    // 相手の IP と ポートを覚える
    bool SetPeer(const char* ipv4, std::uint16_t port);

    // SetPeer の相手に送る
    bool Send(const void* data, int size);

    //====================================//
    ////////////////////////////////////////

    // 戻り値: >0 受信バイト数 / 0 何も届いていない / -1 エラー
    int  Receive(void* buffer, int bufferSize);


    // ip と port はネットワークバイトオーダー
    // ネットワークバイトオーダー = ビッグエンディアン (上の桁のバイトが先)。
    // Intel / AMD の CPU はリトルエンディアン (下の桁が先) なので、そのままでは逆向き。
    // ここでは「Winsock に渡す形のまま」受け取るので、この関数の中では変換しない。

    bool SendTo(std::uint32_t ip, std::uint16_t port, const void* data, int size);

    // 戻り値は Receive と同じ。送り主の ip と port を返す
    // ip と port は参照で受け取り、関数の中で書き込んで返す
    int  ReceiveFrom(void* buffer, int bufferSize, std::uint32_t& ip, std::uint16_t& port);

    // 実際に使っているポート番号 (Open(0) のときの確認用)。開いていなければ 0
    std::uint16_t LocalPort() const;

    // 192.168.0.10 のような文字列を数値 (ネットワークバイトオーダー) に変換する
    // static: ソケットを使わない関数なので、UdpSocket を作らなくても
    // UdpSocket::ParseIPv4(...) と呼べる。
    static bool ParseIPv4(const char* text, std::uint32_t& ip);

    bool IsOpen() const;

    // 最後に起きたエラーの番号 (WSAGetLastError の値)
    int  LastError() const { return lastError_; }

private:
    std::uintptr_t sock_      = ~std::uintptr_t(0); // INVALID_SOCKET と同じ値
    std::uint32_t  peerAddr_  = 0;                  // ネットワークバイトオーダー (1対1用の相手)
    std::uint16_t  peerPort_  = 0;                  // ネットワークバイトオーダー (1対1用の相手)
    bool           hasPeer_   = false;              // SetPeer が呼ばれたか
    int            lastError_ = 0;                  // 最後のエラー番号
};

#endif