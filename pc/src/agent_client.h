#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "proto.h"

class AgentClient {
public:
    AgentClient() = default;
    ~AgentClient();

    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;

    bool Connect(const std::string& host, int port, std::string& err);
    void Disconnect();
    bool Connected() const { return m_sock != 0; }

    bool Hello(s2sc::HelloReply& out, std::string& err);
    bool ListProcs(std::vector<s2sc::ProcEntry>& out, std::string& err);
    bool Attach(uint32_t pid, std::string& err);
    bool Regions(std::vector<s2sc::RegionEntry>& out, std::string& err);
    bool Read(uint64_t addr, void* dst, uint32_t len);
    bool Write(uint64_t addr, const void* src, uint32_t len);
    bool Chain(const std::vector<s2sc::ChainStep>& steps, s2sc::ChainReply& out, std::string& err);
    bool Ping(uint64_t token);

    // Device-side search: resolves an Il2CppClass by name and returns every
    // object whose klass pointer is that class. Walks gigabytes of the target's
    // memory locally, so it takes seconds, not the minutes a PC-side walk would.
    bool ScanClass(const std::string& name, uint32_t max_hits, uint64_t& klass,
                   std::vector<uint64_t>& objs, std::string& err);

    // Device-side scan by object fields: every 8-aligned address whose weapon
    // byte is 1..80 and whose skin id is one of `ids`. One pass, no metadata.
    bool ScanObjects(uint32_t skin_off, uint32_t weapon_off, uint32_t mag_off, uint32_t ammo_off,
                     uint32_t rest_off, const std::vector<uint32_t>& ids,
                     uint32_t max_hits, std::vector<uint64_t>& objs, std::string& err);

    std::string LastError() const { return m_last_error; }

private:
    bool Send(uint16_t op, const void* payload, uint32_t len);
    bool Recv(uint16_t expected_op, std::vector<uint8_t>& out, uint32_t max_len = 64u << 20);
    void SetRecvTimeout(int ms);

    uintptr_t   m_sock = 0;
    std::string m_last_error;
};
