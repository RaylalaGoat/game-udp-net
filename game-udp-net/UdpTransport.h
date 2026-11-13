#ifndef _UQPTRANSPORT_H_
#define _UQPTRANSPORT_H_

#include "ITransport.h" // 継承する基底クラス
#include "UdpSocket.h"  // メンバとして持つクラス (UdpSocket.h は winsock2.h を include しないので安全)

#include <cstdint>

// =========================================================
// UqpTransport.h
//
// 制作者:h		日付：
// 
// UdpSocket (Winsock を使うクラス) を、ITransport の 形 に合わせるクラス。
// NetSession は ITransport しか知らないので、UdpSocket を直接は使えない。
// =========================================================

namespace net
{
    //==============================================================
    // Winsock (UDP) 版の送受信の層
    // 使う前に WinsockContext を作っておくこと。
    //==============================================================
    //これ以上このクラスを継承させない
    class UdpTransport final : public ITransport
    {
    public:
        // Open() を読んだとき
        UdpTransport() = default;

        // デストラクタはコンパイラが自動で作る。
        // 書かなくてもソケットは必ず閉じられる

        // ホストは決まったポート、参加する側は 0 (OS に任せる)
        bool Open(std::uint16_t localPort);
        void Close();

        // 全部 socket_ に「そのまま聞いて返すだけ」
        bool IsOpen() const { return socket_.IsOpen(); }

        std::uint16_t LocalPort() const { return socket_.LocalPort(); }
        int           LastError() const { return socket_.LastError(); }

        bool SendTo(const NetAddress& to, const void* data, int size) override;
        int  ReceiveFrom(void* buffer, int bufferSize, NetAddress& from) override;

        // "192.168.0.10" と 50000 から NetAddress を作る
        // NetAddress の中身 (どちらのバイト順で入っているか) を知っているのはこのクラスだけなので、
        // NetAddress を作る仕事もこのクラスに置く
        static bool MakeAddress(const char* ipv4, std::uint16_t port, NetAddress& out);

    private:
        UdpSocket socket_; // 実際に通信するソケット。UdpTransport が持ち主で、一緒に作られ一緒に破棄される
    };
}

#endif