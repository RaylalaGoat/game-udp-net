#pragma once
#include "NetTypes.h"

#include <cstdint>
#include <cstring>

// =========================================================
// NetTypes.h
//
// 制作者:h		日付：
// 
// 1: パケットに付ける印と大きさの上限
// 2: パケットの種類の番号
// 3: 通し番号の新旧の比べ方 (番号が一周しても正しく比べる)
// 4: ByteWriter (値をバイト列に書く)
// 5: ByteReader (バイト列から値を読む)
// NetSession.cpp だけが include する。ゲーム側は使わない。
// =========================================================

// ==============================================================
// 通信モジュール内部で使うパケットの形 (ゲーム側は使わない)
// 値は1つずつリトルエンディアンで書き出す (構造体をそのまま送らない)。
// ==============================================================
// namespace net::detail
// C++17 の書き方
// 内部用外から使わない
namespace net::detail
{
    // ----------------------------------------------------------
    // 1: 印と上限
    // ----------------------------------------------------------

    // 全てのパケットの先頭に書く「このゲームのパケットです」という印
    // 先頭2バイトがこれでなければ捨てる。
    // 偶然一致 その後の種類・長さのチェックでもほとんど弾ける)
    constexpr std::uint16_t kProtocolMagic   = 0x4E47;

    // 取り決めのバージョン。パケットの形を変えたら 2, 3 ... と上げる。
    constexpr std::uint8_t  kProtocolVersion = 1;

    // 1個のパケットの最大バイト数。
    constexpr int           kMaxPacketSize   = 1200;

    // ----------------------------------------------------------
    // 2: パケットの種類
    // ----------------------------------------------------------

    enum class MsgType : std::uint8_t
    {
        JoinRequest = 1, // 参加 → ホスト        : 参加したい
        JoinAccept  = 2, // ホスト → 参加者      : ID を割り当てた
        JoinRefuse  = 3, // ホスト → 参加者      : 参加できない
        Leave       = 4, // どちらからでも        : 抜ける
        ClientState = 5, // 参加者 → ホスト      : 自分の状態
        WorldState  = 6, // ホスト → 参加者      : 全員分の状態
        Reliable    = 7, // どちらからでも        : 確実に届けるメッセージ
        Ack         = 8, // どちらからでも        : Reliable を受け取った返事
    };

    // Reliable の中身の種類
    // Reliable 1つ1つのメッセージに付ける種類
    enum class ReliableKind : std::uint8_t
    {
        Roster    = 1, // ホスト → 参加者 : 参加者一覧・陣営・試合の状態
        GameEvent = 2, // ゲームが決めるイベント
    };

    // 参加を断る理由。JoinRefuse パケットに入れる。
    // 参加者はこれを DisconnectReason (JoinRefusedFull / JoinRefusedInMatch) に変換してゲームに伝える。
    enum class RefuseReason : std::uint8_t
    {
        Full    = 1,
        InMatch = 2,
    };

    // ----------------------------------------------------------
    // 3: 通し番号の新旧
    // ----------------------------------------------------------
    // 通し番号 (シーケンス番号) は 16bit なので、65535 の次は 0 に戻る (一周する)。
    // a > b で比べると、65535 → 0 になった瞬間に「0 の方が古い」と間違える。
    
    // a が b より新しい番号か (番号が一周しても正しく判定する)
    inline bool SeqNewer16(std::uint16_t a, std::uint16_t b)
    {
        return static_cast<std::int16_t>(static_cast<std::uint16_t>(a - b)) > 0;
    }

    // 32bit 版。状態パケット (ClientState / WorldState) の通し番号の比較に使う。
    // 順番が入れ替わって届いた「古い状態」を捨てるため
    // (新しい状態の後に古い状態を使うと、キャラが一瞬戻って見える)
    inline bool SeqNewer32(std::uint32_t a, std::uint32_t b)
    {
        return static_cast<std::int32_t>(a - b) > 0;
    }

    // ----------------------------------------------------------
    // 4: バイト列への書き込み。容量を超えたら Ok() が false になる
    // ----------------------------------------------------------
    class ByteWriter
    {
    public:
        ByteWriter(std::uint8_t* buffer, int capacity) : buffer_(buffer), capacity_(capacity) {}

        void U8(std::uint8_t v)
        {
            if (Reserve(1))
            {
                buffer_[size_++] = v;
            }
        }

        void U16(std::uint16_t v)
        {
            if (Reserve(2))
            {
                buffer_[size_++] = static_cast<std::uint8_t>(v & 0xFF);
                buffer_[size_++] = static_cast<std::uint8_t>(v >> 8);
            }
        }

        void U32(std::uint32_t v)
        {
            if (Reserve(4))
            {
                for (int i = 0; i < 4; ++i)
                {
                    buffer_[size_++] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
                }
            }
        }

        void F32(float v)
        {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v, sizeof(bits));
            U32(bits);
        }

        void Bytes(const void* data, int n)
        {
            if (n < 0)
            {
                ok_ = false;
                return;
            }
            if (Reserve(n))
            {
                if (n > 0)
                {
                    std::memcpy(buffer_ + size_, data, static_cast<std::size_t>(n));
                }
                size_ += n;
            }
        }

        const std::uint8_t* Data() const { return buffer_; }
        int  Size() const { return size_; }
        bool Ok() const { return ok_; }

    private:
        bool Reserve(int n)
        {
            if (!ok_ || n > capacity_ - size_)
            {
                ok_ = false;
                return false;
            }
            return true;
        }

        std::uint8_t* buffer_;
        int           capacity_;
        int           size_ = 0;
        bool          ok_   = true;
    };

    // ----------------------------------------------------------
    // 5: バイト列からの読み込み。足りなければ Ok() が false になり、0 を返す
    // ----------------------------------------------------------
    class ByteReader
    {
    public:
        ByteReader(const std::uint8_t* data, int size) : data_(data), size_(size) {}

        std::uint8_t U8()
        {
            if (!Check(1))
            {
                return 0;
            }
            return data_[pos_++];
        }

        std::uint16_t U16()
        {
            if (!Check(2))
            {
                return 0;
            }
            const std::uint16_t v = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
            pos_ += 2;
            return v;
        }

        std::uint32_t U32()
        {
            if (!Check(4))
            {
                return 0;
            }
            std::uint32_t v = 0;
            for (int i = 0; i < 4; ++i)
            {
                v |= static_cast<std::uint32_t>(data_[pos_ + i]) << (8 * i);
            }
            pos_ += 4;
            return v;
        }

        float F32()
        {
            const std::uint32_t bits = U32();
            float v = 0.0f;
            std::memcpy(&v, &bits, sizeof(v));
            return v;
        }

        void Bytes(void* out, int n)
        {
            if (n < 0 || !Check(n))
            {
                ok_ = false;
                return;
            }
            if (n > 0)
            {
                std::memcpy(out, data_ + pos_, static_cast<std::size_t>(n));
            }
            pos_ += n;
        }

        int  Remaining() const { return size_ - pos_; }
        bool Ok() const { return ok_; }

    private:
        bool Check(int n)
        {
            if (!ok_ || n > size_ - pos_)
            {
                ok_ = false;
                return false;
            }
            return true;
        }

        const std::uint8_t* data_;
        int                 size_;
        int                 pos_ = 0;
        bool                ok_  = true;
    };
}