// 通信モジュールの動作確認ツール (コンソール)
//
// 1つ目で h (ホスト)、残りで j (参加) を選ぶ。
// 操作:
//   全員       : g = テスト用イベントを送る / q = 終了
//   ホストのみ : r = 1人側(Solo)を次の人に交代 / s = 試合開始 / e = 試合終了
#include "NetSession.h"
#include "UdpSocket.h"
#include "UdpTransport.h"

#include <array>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    constexpr std::uint16_t kHostPort       = 50000;
    constexpr std::uint16_t kEventTestPing  = 1; // テスト用イベントの種類

    // custom に載せるテスト用の状態
    struct TestCustom
    {
        std::uint16_t frame;
        std::uint8_t  hp;
    };

    // テスト用イベントの中身
    struct TestPing
    {
        std::int32_t counter;
    };

    int Fail(const char* what, int code)
    {
        std::cout << what << " failed: " << code << "\nPress any key to exit...\n";
        _getch();
        return 1;
    }

    const char* ToString(net::Team team)
    {
        switch (team)
        {
        case net::Team::Solo:  return "Solo ";
        case net::Team::Squad: return "Squad";
        default:               return "-    ";
        }
    }

    const char* ToString(net::SessionState state)
    {
        switch (state)
        {
        case net::SessionState::Idle:         return "Idle";
        case net::SessionState::Connecting:   return "Connecting";
        case net::SessionState::Lobby:        return "Lobby";
        case net::SessionState::InMatch:      return "InMatch";
        case net::SessionState::Disconnected: return "Disconnected";
        }
        return "?";
    }

    const char* ToString(net::DisconnectReason reason)
    {
        switch (reason)
        {
        case net::DisconnectReason::None:               return "None";
        case net::DisconnectReason::Left:               return "Left";
        case net::DisconnectReason::Timeout:            return "Timeout";
        case net::DisconnectReason::HostLeft:           return "HostLeft";
        case net::DisconnectReason::JoinRefusedFull:    return "JoinRefusedFull";
        case net::DisconnectReason::JoinRefusedInMatch: return "JoinRefusedInMatch";
        case net::DisconnectReason::JoinTimeout:        return "JoinTimeout";
        case net::DisconnectReason::TransportError:     return "TransportError";
        }
        return "?";
    }

    void PrintEvent(const net::NetEvent& e)
    {
        switch (e.type)
        {
        case net::NetEventType::Connected:
            std::printf("[event] Connected (my id = %d)\n", e.player);
            break;
        case net::NetEventType::Disconnected:
            std::printf("[event] Disconnected (%s)\n", ToString(e.reason));
            break;
        case net::NetEventType::PlayerJoined:
            std::printf("[event] PlayerJoined id=%d\n", e.player);
            break;
        case net::NetEventType::PlayerLeft:
            std::printf("[event] PlayerLeft id=%d (%s)\n", e.player, ToString(e.reason));
            break;
        case net::NetEventType::TeamChanged:
            std::printf("[event] TeamChanged id=%d\n", e.player);
            break;
        case net::NetEventType::MatchStarted:
            std::printf("[event] MatchStarted\n");
            break;
        case net::NetEventType::MatchEnded:
            std::printf("[event] MatchEnded\n");
            break;
        case net::NetEventType::GameEvent:
        {
            TestPing ping = {};
            if (e.gameEventType == kEventTestPing && e.Read(ping))
            {
                std::printf("[event] Ping #%d from id=%d\n", static_cast<int>(ping.counter), e.player);
            }
            else
            {
                std::printf("[event] GameEvent type=%d from id=%d (%d bytes)\n",
                            e.gameEventType, e.player, static_cast<int>(e.payload.size()));
            }
            break;
        }
        }
    }

    // ホスト: 1人側を次のプレイヤーに交代し、他は全員複数人側にする
    void RotateSolo(net::NetSession& session)
    {
        std::array<net::PlayerInfo, net::kMaxPlayers> players;
        const int count = session.GetPlayers(players);
        if (count == 0)
        {
            return;
        }

        int current = -1;
        for (int i = 0; i < count; ++i)
        {
            if (players[i].team == net::Team::Solo)
            {
                current = i;
            }
        }
        const int next = (current + 1) % count;
        for (int i = 0; i < count; ++i)
        {
            session.SetTeam(players[i].id, (i == next) ? net::Team::Solo : net::Team::Squad);
        }
    }

    void PrintPlayers(const net::NetSession& session)
    {
        std::array<net::PlayerInfo, net::kMaxPlayers> players;
        const int count = session.GetPlayers(players);
        std::printf("---- %s  match=%d  players=%d ----\n",
                    ToString(session.State()), session.MatchNumber(), count);

        for (int i = 0; i < count; ++i)
        {
            const net::PlayerInfo& p = players[i];
            std::printf("  id=%d %s %s%s", p.id, ToString(p.team), p.isHost ? "[host]" : "      ",
                        p.isLocal ? "[me]" : "    ");
            if (!p.hasState)
            {
                std::printf("  (no state yet)\n");
                continue;
            }
            const float* pos = p.state.transform.position;
            const float* rot = p.state.transform.rotation;
            const float  yaw = 2.0f * std::atan2(rot[1], rot[3]);
            TestCustom custom = {};
            const bool hasCustom = p.state.GetCustom(custom);
            std::printf("  pos=(%6.2f, %6.2f, %6.2f) yaw=%5.2f", pos[0], pos[1], pos[2], yaw);
            if (hasCustom)
            {
                std::printf("  hp=%d", custom.hp);
            }
            std::printf("\n");
        }
    }
}

int main()
{
    WinsockContext winsock; // 最初に作る → 最後に破棄される
    if (!winsock.IsOk())
    {
        return Fail("WSAStartup", 0);
    }

    std::string line;
    for (;;)
    {
        std::cout << "h = host, j = join: ";
        std::getline(std::cin, line);
        if (line == "h" || line == "j")
        {
            break;
        }
    }
    const bool isHost = (line == "h");

    net::NetAddress hostAddress;
    if (!isHost)
    {
        std::cout << "host IP (Enter = 127.0.0.1): ";
        std::getline(std::cin, line);
        const std::string ip = line.empty() ? "127.0.0.1" : line;
        if (!net::UdpTransport::MakeAddress(ip.c_str(), kHostPort, hostAddress))
        {
            return Fail("MakeAddress (type the IP as numbers)", 0);
        }
    }

    // ホストは決まったポート、参加する側は OS に任せる
    net::UdpTransport transport;
    if (!transport.Open(isHost ? kHostPort : 0))
    {
        // 10048 = ポートが使用中 (ホストが既に起動している)
        return Fail("Open", transport.LastError());
    }
    std::printf("local port %d\n", transport.LocalPort());

    net::NetSession session(transport); // transport より後に作る → 先に破棄される
    if (isHost)
    {
        session.Host();
        std::printf("hosting on port %d.  keys: r=rotate Solo, s=start, e=end, g=ping, q=quit\n", kHostPort);
    }
    else
    {
        session.Join(hostAddress);
        std::printf("joining...  keys: g=ping, q=quit\n");
    }

    using Clock = std::chrono::steady_clock;
    const auto start      = Clock::now();
    auto       nextPrint  = start;
    int        pingCount  = 0;
    std::uint16_t frame   = 0;

    for (;;)
    {
        // ---- 入力 ----
        if (_kbhit())
        {
            const int key = _getch();
            if (key == 'q')
            {
                break;
            }
            if (key == 'g')
            {
                const TestPing ping = { ++pingCount };
                if (session.SendGameEvent(kEventTestPing, ping))
                {
                    std::printf("sent ping #%d\n", pingCount);
                }
                else
                {
                    std::printf("cannot send ping (not connected)\n");
                }
            }
            if (session.IsHost())
            {
                if (key == 'r') { RotateSolo(session); }
                if (key == 's') { std::printf(session.StartMatch() ? "match started\n" : "cannot start (need 1 Solo + 1-3 Squad)\n"); }
                if (key == 'e') { std::printf(session.EndMatch() ? "match ended\n" : "not in match\n"); }
            }
        }

        // ---- 通信 ----
        session.Update();

        net::NetEvent e;
        while (session.PollEvent(e))
        {
            PrintEvent(e);
        }
        if (session.State() == net::SessionState::Disconnected)
        {
            break;
        }

        // ---- 自分の状態 (テスト用に円を描いて動く) ----
        const float t   = std::chrono::duration<float>(Clock::now() - start).count();
        const int   id  = (session.LocalPlayerId() == net::kInvalidPlayerId) ? 0 : session.LocalPlayerId();
        const float yaw = t + static_cast<float>(id) * 1.5708f;

        net::PlayerState state;
        state.transform.position[0] = std::cos(yaw) * 5.0f;
        state.transform.position[2] = std::sin(yaw) * 5.0f;
        state.transform.rotation[1] = std::sin(yaw * 0.5f); // y 軸まわりの回転
        state.transform.rotation[3] = std::cos(yaw * 0.5f);
        state.SetCustom(TestCustom{ ++frame, static_cast<std::uint8_t>(100 - id * 10) });
        session.SetLocalState(state);

        // ---- 表示 (1秒ごと) ----
        if (Clock::now() >= nextPrint)
        {
            nextPrint += std::chrono::seconds(1);
            PrintPlayers(session);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    session.Leave(); // 待っている間に相手がタイムアウトしないよう、先に抜けたことを伝える
    std::printf("Press any key to exit...\n");
    _getch();
    return 0; // session → transport → winsock の順に自動で後始末される
}
