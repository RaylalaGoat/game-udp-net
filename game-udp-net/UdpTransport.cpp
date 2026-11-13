#include "UdpTransport.h"

#include <winsock2.h>

// htons を使うためだけに include する。
// .cpp の中なので、ゲーム側の windows.h とは衝突しない

// =========================================================
// UdqTransport.cpp
//
// 制作者:h		日付：
// 
// UdpTransport の関数の中身。全ての関数が「UdpSocket に任せる」か「住所の形を変換する」
// =========================================================

namespace net
{
    // 開く・閉じるは、そのまま UdpSocket に任せる
    bool UdpTransport::Open(std::uint16_t localPort)
    {
        return socket_.Open(localPort);
    }

    void UdpTransport::Close()
    {
        socket_.Close();
    }

    //----------------------------------------------------------
    // 送信: NetAddress → (ip, port) に変換して UdpSocket に渡す
    //----------------------------------------------------------

    bool UdpTransport::SendTo(const NetAddress& to, const void* data, int size)
    {
        // NetAddress::value は 64bit (将来的に Steam の SteamID も入るように大きくしてある)。
        // UDP (IPv4) の住所は 32bit なので、下の 32bit だけを取り出す。
        // port はネットワークバイトオーダーのまま入っているので、変換せずにそのまま渡す。
        return socket_.SendTo(static_cast<std::uint32_t>(to.value), to.port, data, size);
    }

    //----------------------------------------------------------
    // 受信: UdpSocket から (ip, port) を受け取り、NetAddress に入れて返す
    //----------------------------------------------------------

    int UdpTransport::ReceiveFrom(void* buffer, int bufferSize, NetAddress& from)
    {
        std::uint32_t ip   = 0;
        std::uint16_t port = 0;
        const int n = socket_.ReceiveFrom(buffer, bufferSize, ip, port);

        // 受信できたときだけ from に書き込む。
        // 何も届いていない (0) やエラー (-1) のときは、from を触らない
        if (n > 0)
        {
            from.value = ip;
            from.port  = port;
        }
        // 戻り値の意味 (>0 / 0 / -1) は UdpSocket と ITransport で同じにしてあるので、そのまま返す。
        return n;
    }

    //----------------------------------------------------------
    // 住所を作る: "192.168.0.10" と 50000 → NetAddress
    //----------------------------------------------------------
    bool UdpTransport::MakeAddress(const char* ipv4, std::uint16_t port, NetAddress& out)
    {
        std::uint32_t ip = 0;
        if (!UdpSocket::ParseIPv4(ipv4, ip))
        {
            return false; // 形式が正しくない
        }

        // ip は ParseIPv4 が既にネットワークバイトオーダーで返している。
        // port は人が読む普通の数 (50000) で受け取っているので、htons でネットワークバイトオーダーにする。

        //   NetAddress は「Winsock に渡す形のまま」 (NetTypes.h)。
        //   作るときに1回だけ変換しておけば、送るたび/比べるたびに変換しなくてよい。
        //   受信した住所 (ReceiveFrom) も同じ形なので、== でそのまま比べられる。
        //   もしここで htons を忘れると、50000 (0xC350) が 0x50C3 = 20675 番ポートとして扱われ、
        //   相手に届かない。
        out.value = ip;
        out.port  = htons(port);
        return true;
    }
}
