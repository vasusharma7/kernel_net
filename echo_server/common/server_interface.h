#pragma once

#include <cstdint>

// Every server flavor must implement this interface.
class ServerInterface {
public:
    virtual ~ServerInterface() = default;
    virtual void run()         = 0;
    virtual void shutdown()    = 0;
};