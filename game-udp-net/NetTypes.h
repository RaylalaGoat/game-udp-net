#ifndef _NETTYPES_H_
#define _NETTYPES_H_

#include <cstdint>
// =========================================================
// NetTypes.h
//
// 制作者:h		日付：

// 通信モジュールで共通に使う型と定数「全員が使う共通の言葉」
//==========================================================

// 名前空間netの中に入れる。
// 区別できるようにして 名前の衝突を防ぐため
// net::

namespace net
{
    // =========================================================
    // 1: 人数 と ID
    // =========================================================
    
    // 最大人数 (1対1〜1対3)
    constexpr int kMaxPlayers = 4;

    // プレイヤーID (0〜3)。ホストは必ず 0
    using PlayerId = std::uint8_t;
    constexpr PlayerId kInvalidPlayerId = 0xFF; // 255 無効
    constexpr PlayerId kHostPlayerId    = 0;    // 0はホスト

    // =========================================================
    // 2: 通信相手の住所
    // =========================================================


    // 通信相手の住所。
    // UDP では IPv4 とポート、Steamworks では SteamID を入れる想定。
    // ゲーム側は中身を気にせず、コピーと比較だけを行う。
    struct NetAddress
    {
        std::uint64_t value = 0; // UDP: IPv4 (ネットワークバイトオーダー)
        std::uint16_t port  = 0; // UDP: ポート (ネットワークバイトオーダー)

        // 住所が同じかどうか 受け取ったパケットが どの参加者なのか探すとき使う
        bool operator==(const NetAddress& other) const { return value == other.value && port == other.port; }
        bool operator!=(const NetAddress& other) const { return !(*this == other); }
    };

    // =========================================================
    // 3: 陣営
    // =========================================================

    // 陣営
    enum class Team : std::uint8_t
    {
        None  = 0, // 未設定
        Solo  = 1, // ゴースト
        Squad = 2, // ハンター
    };
    // 受け取ったとき (NetSession.cpp) IsValidTeam()
    // 0～2の範囲か確かめてから enum に返す

    // =========================================================
    // 4: セッション状態
    // =========================================================

    // セッションの状態
    // 自分の PC の中だけで使う
    enum class SessionState : std::uint8_t
    {
        Idle,         // 何もしていない
        Connecting,   // ホストに参加を申し込んでいる
        Lobby,        // 待機中 (試合前)
        InMatch,      // 試合中
        Disconnected, // 切断された (Leave() で Idle に戻す)
    };
    // Disconnected 自分の意思とは関係なく切れたとき(無応答 / ホスト退出 / 参加拒否)

    // =========================================================
    // 切断 / 退出の理由
    // =========================================================

    enum class DisconnectReason : std::uint8_t
    {
        None,
        Left,               // 自分から抜けた
        Timeout,            // 一定時間データが届かなかった
        HostLeft,           // ホストが抜けた
        JoinRefusedFull,    // 満員で参加できなかった
        JoinRefusedInMatch, // 試合中で参加できなかった
        JoinTimeout,        // ホストから返事がなかった
        TransportError,     // 送受信の層でエラーが起きた
    };
}

#endif