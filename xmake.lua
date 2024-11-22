set_project("uring_exec")
set_languages("c++20")
set_targetdir("build")

-- xmake-repo is outdated. (stdexec is changing too frequently.)
add_requires("stdexec main", { 
    alias = "stdexec_latest",
    system = false,
    configs = { repo = "github:NVIDIA/stdexec" }
})
add_requires("liburing", {
    system = false,
    configs = { repo = "github:axboe/liburing" }
})
add_requires("asio", {
    system = false,
    configs = { repo = "github:chriskohlhoff/asio" } 
})

add_includedirs("include")
add_cxxflags("-Wall", "-Wextra", "-g")
add_links("uring", "pthread")

local examples_targets = {}
local benchmarks_targets = {}

-- For example, xmake build timer
for _, file in ipairs(os.files("examples/*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        add_files(file)
        add_packages("stdexec_latest", "liburing")
    table.insert(examples_targets, name)
end

-- For example, xmake build ping
for _, file in ipairs(os.files("benchmarks/*.cpp")) do
    local name = path.basename(file)
    target(name)
        set_kind("binary")
        add_files(file)
        add_cxxflags("-DASIO_HAS_IO_URING -DASIO_DISABLE_EPOLL")
        add_cxxflags("-O3")
        add_packages("stdexec_latest", "liburing", "asio")
        table.insert(benchmarks_targets, name)
end

target("examples")
    set_kind("phony")
    add_deps(table.unpack(examples_targets))

-- Usage: xmake run benchmarks --server [uring_exec|asio]
target("benchmarks")
    set_kind("phony")
    add_deps(table.unpack(benchmarks_targets))
    on_run(function (target)
        import("core.base.option")
        local script_path = "./benchmarks/pingpong.py"
        local args = option.get("arguments") or {}
        table.insert(args, 1, "--xmake")
        table.insert(args, 2, "y")
        os.execv("python3", {script_path, unpack(args)})
    end)

target("clean")
    on_clean(function ()
        os.rm("build")
    end)
    set_kind("phony")
