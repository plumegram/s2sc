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

    std::string LastError() const { return m_last_error; }

private:
    bool Send(uint16_t op, const void* payload, uint32_t len);
    bool Recv(uint16_t expected_op, std::vector<uint8_t>& out, uint32_t max_len = 64u << 20);

    uintptr_t   m_sock = 0;
    std::string m_last_error;
};
