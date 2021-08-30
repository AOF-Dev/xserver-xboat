srcs = [
    'xboat.c',
    'xboatinit.c',
    'xboat_draw.c',
    'hostboat.c',
]

xboat_dep = [
    common_dep,
    dependency('boat'),
    dependency('android'),
]

xboat_glamor = []
if build_glamor
    srcs += 'xboat_glamor_egl.c'
    if build_xv
        srcs += 'xboat_glamor_xv.c'
    endif
    xboat_glamor += glamor
    xboat_glamor += glamor_egl_stubs
    xboat_dep += epoxy_dep
endif

executable(
    'Xboat',
    srcs,
    include_directories: [
        inc,
        include_directories('../src')
    ],
    dependencies: xboat_dep,
    link_with: [
        libxserver_main,
        libxserver_exa,
        xboat_glamor,
        kdrive,
        libxserver_fb,
        libxserver,
        libxserver_config,
        libxserver_xkb_stubs,
        libxserver_xi_stubs,
        libxserver_glx,
        libglxvnd,
    ],
    install: true,
)

xboat_man = configure_file(
    input: 'man/Xboat.man',
    output: 'Xboat.1',
    configuration: manpage_config,
)
install_man(xboat_man)
