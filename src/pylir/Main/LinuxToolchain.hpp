
#pragma once

#include "Toolchain.hpp"

namespace pylir
{
class LinuxToolchain : public Toolchain
{
protected:
    Stdlib defaultStdlib() const override
    {
        return Stdlib::libstdcpp;
    }

    RTLib defaultRTLib() const override
    {
        return RTLib::libgcc;
    }

public:
    explicit LinuxToolchain(const llvm::Triple& triple, const cli::CommandLine& commandLine);

    bool link(const cli::CommandLine& commandLine, llvm::StringRef objectFile) const override;
};
} // namespace pylir