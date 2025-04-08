load("//build/kernel/kleaf:kernel.bzl", "ddk_module")
load("//build/bazel_common_rules/dist:dist.bzl", "copy_to_dist_dir")

def define_modules_for_specific_variants():
    target = "sdxkova"
    variants = ["debug-defconfig", "perf-defconfig"]
    for variant in variants:
        _define_modules(target, variant)

def _define_modules(target, variant):
    rule_base = "{}_{}".format(target, variant)

    ddk_module(
        name = "{}_qca_nss_sfe".format(rule_base),
        out = "qca-nss-sfe.ko",
        srcs = native.glob([
            "sfe.c",
            "sfe_init.c",
            "sfe_ipv4.c",
            "sfe_ipv4_esp.c",
            "sfe_ipv4_gre.c",
            "sfe_ipv4_icmp.c",
            "sfe_ipv4_multicast.c",
            "sfe_ipv4_pppoe_br.c",
            "sfe_ipv4_tcp.c",
            "sfe_ipv4_udp.c",
            "sfe_ipv4_tun6rd.c",
            "sfe_ipv6.c",
            "sfe_ipv6_esp.c",
            "sfe_ipv6_frag.c",
            "sfe_ipv6_gre.c",
            "sfe_ipv6_icmp.c",
            "sfe_ipv6_multicast.c",
            "sfe_ipv6_tcp.c",
            "sfe_ipv6_udp.c",
            "sfe_pppoe.c",
            "sfe_pppoe_mgr.c",
            "sfe_pppoe_br.c",
            "sfe_vlan.c",
	    "sfe_ipv6_etherip.c",
	    "sfe_ipv6_pppoe_br.c",
        ]),
        hdrs = [
		"exports/sfe_api.h",
	],
        includes = [
		".",
		"exports",  # Correct relative path
	],
        deps = [
            ":sfe_headers",
            "//msm-kernel:all_headers",
        ],
        kernel_build = "//msm-kernel:{}".format(rule_base),
        copts = [
            "-DSFE_SUPPORT_IPV4=1",
            "-DSFE_SUPPORT_IPV6=1",
            #"-DSFE_SUPPORT_PPPOE=1",
            #"-DSFE_RFS_SUPPORTED=1",
            "-DSFE_PROCESS_LOCAL_OUT=1",
            "-DSFE_BRIDGE_VLAN_FILTERING_ENABLE=1",
            #"-DSFE_IPSEC_ENABLE=1",
            #"-DSFE_GRE_ENABLE=1",
            #"-DSFE_DSLLITE_ENABLE=1",
	    "-Wno-parentheses-equality",
	    "-Wno-strict-prototypes",     # Suppress strict prototypes warning
	    "-Wno-uninitialized",         # Suppress uninitialized variable warning
        ],
        visibility = ["//visibility:public"],
    )

    copy_to_dist_dir(
        name = "{}_qca_nss_sfe_module_dist".format(rule_base),
        data = [":{}_qca_nss_sfe".format(rule_base)],
        dist_dir = "out/target/product/{}/dlkm/lib/modules/".format(target),
        flat = True,
        wipe_dist_dir = False,
        allow_duplicate_filenames = False,
        mode_overrides = {"**/*": "644"},
        log = "info",
    )

def define_qca_nss_sfe():
    define_modules_for_specific_variants()
