add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", { outputdir = ".", lsp = "clangd" })

set_encodings("utf-8")
set_optimize("aggressive")

target("coreclr-tracer")
    set_kind("shared")
    set_languages("cxx20")
    set_arch("x64")

    add_files("src/**/*.cpp")
    add_files("src/*.cpp")

    if is_plat("windows") then
        add_files("src/*.def")
    end

    if is_plat("windows") then
        add_defines("WIN32")
        add_defines("_WIN32")
    else
        add_defines("HOST_AMD64")
    end

    add_includedirs("vendor/dotnet-runtime/src/coreclr/pal/prebuilt/inc")
    if is_plat("linux") then
        add_includedirs("vendor/dotnet-runtime/src/coreclr/pal/inc")
        add_includedirs("vendor/dotnet-runtime/src/coreclr/pal/inc/rt")
    end
    add_includedirs("vendor/dotnet-runtime/src/coreclr/inc")
    add_includedirs("vendor/dotnet-runtime/src/coreclr")
    add_includedirs("vendor/dotnet-runtime/src/native")

    before_build(function (target)
        local dir = path.join(os.projectdir(), "vendor", "dotnet-runtime")
        if os.isdir(dir) then
            return
        end
        try {
            function ()
                os.vrunv("git", {"clone", "--depth=1", "--branch", "v10.0.12", "--filter=blob:none", "--sparse",
                    "https://github.com/dotnet/runtime.git", dir})
                os.vrunv("git", {"-C", dir, "sparse-checkout", "set",
                    "src/coreclr/pal/prebuilt/inc", "src/coreclr/pal/inc", "src/coreclr/inc", "src/native"})
            end,
            catch {
                function (errors)
                    os.tryrm(dir)
                    raise(errors)
                end
            }
        }
    end)
