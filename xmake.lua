set_project("xxfs")
set_version("0.3.0")
set_languages("c11")

add_includedirs("lib")

option("readline", {default = true, description = "Enable readline support"})
option("profile", {default = false, description = "Enable CPU profiling"})

target("xxfs-lib", function()
    set_kind("static")
    add_files("lib/xxfs.c", "lib/xxfs_os.c")
    if has_config("profile") then
        add_defines("XXFS_PROFILE")
    end
end)

target("mkfs.xxfs", function()
    set_kind("binary")
    add_files("mkfs/mkfs.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
end)

target("fsck.xxfs", function()
    set_kind("binary")
    add_files("fsck/fsck.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
end)

target("xxfs-shell", function()
    set_kind("binary")
    add_files("shell/shell.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
    if has_config("readline") then
        add_defines("HAVE_READLINE")
        add_syslinks("readline")
    end
end)

target("xxfs-bench", function()
    set_kind("binary")
    add_files("bench/bench.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
    if has_config("profile") then
        add_defines("XXFS_PROFILE")
    end
end)

target("posix-bench", function()
    set_kind("binary")
    add_files("bench/posix_bench.c")
    add_syslinks("pthread")
end)

target("read-bench", function()
    set_kind("binary")
    add_files("bench/read_bench.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
    set_default(false)
end)

target("test-basic", function()
    set_kind("binary")
    add_files("test/test_basic.c")
    add_deps("xxfs-lib")
    add_syslinks("pthread")
    set_default(false)
end)
