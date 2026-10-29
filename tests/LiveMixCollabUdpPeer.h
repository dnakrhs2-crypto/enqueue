#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include "CollabProtocol.h"
#include <cstring>

namespace gocue::tests::collabtest
{
// Read the TTL delivered by Winsock, rather than a transport implementation detail.
struct UdpPeer
{
    UdpPeer()
    {
        WSADATA data {}; WSAStartup (MAKEWORD (2, 2), &data);
        socket = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in local {}; local.sin_family = AF_INET; local.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        if (bind (socket, (const sockaddr*) &local, sizeof (local)) != 0) return;
        int size = sizeof (local); getsockname (socket, (sockaddr*) &local, &size); port = ntohs (local.sin_port);
        size = sizeof (defaultTtl);
        if (getsockopt (socket, IPPROTO_IP, IP_TTL, (char*) &defaultTtl, &size) != 0) return;
        const DWORD enabled = 1;
        if (setsockopt (socket, IPPROTO_IP, IP_RECVTTL, (const char*) &enabled, sizeof (enabled)) != 0) return;
        GUID id = WSAID_WSARECVMSG; DWORD bytes = 0;
        if (WSAIoctl (socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &id, sizeof (id), &receiveMessage,
                      sizeof (receiveMessage), &bytes, nullptr, nullptr) != 0) return;
        u_long nonblocking = 1; ioctlsocket (socket, FIONBIO, &nonblocking); ready = true;
    }
    ~UdpPeer() { closesocket (socket); WSACleanup(); }
    bool receive (livemix::collab::Kind kind, int& ttl)
    {
        if (! ready) return false;
        using namespace livemix::collab;
        std::array<uint8_t, maxDatagram> data {};
        alignas (WSACMSGHDR) char control[WSA_CMSG_SPACE (sizeof (DWORD))] {};
        WSABUF buffer { (ULONG) data.size(), (char*) data.data() };
        sockaddr_in from {};
        WSAMSG message {}; message.name = (sockaddr*) &from; message.namelen = sizeof (from);
        message.lpBuffers = &buffer; message.dwBufferCount = 1; message.Control = { sizeof (control), control };
        DWORD bytes = 0;
        if (receiveMessage (socket, &message, &bytes, nullptr, nullptr) != 0) return false;
        Header h; const uint8_t* payload = nullptr;
        if (! readPacket (data.data(), bytes, h, payload) || h.kind != kind) return false;
        ttl = -1;
        for (auto* c = WSA_CMSG_FIRSTHDR (&message); c != nullptr; c = WSA_CMSG_NXTHDR (&message, c))
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_TTL)
            {
                DWORD value = 0; std::memcpy (&value, WSA_CMSG_DATA (c), sizeof (value)); ttl = (int) value;
            }
        return true;
    }
    void send (int portTo, livemix::collab::Header h, const uint8_t* payload = nullptr)
    {
        using namespace livemix::collab;
        std::array<uint8_t, maxDatagram> data {};
        const auto size = writePacket (data.data(), data.size(), h, payload);
        sockaddr_in to {}; to.sin_family = AF_INET; to.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
        to.sin_port = htons ((u_short) portTo);
        sendto (socket, (const char*) data.data(), (int) size, 0, (const sockaddr*) &to, sizeof (to));
    }
    SOCKET socket = INVALID_SOCKET;
    LPFN_WSARECVMSG receiveMessage = nullptr;
    int port = 0, defaultTtl = 0;
    bool ready = false;
};
}
