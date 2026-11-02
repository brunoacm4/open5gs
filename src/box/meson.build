# PANIC: boxes that run several NFs in one process. Built with -Dbox=true.

amfsmf_sources = files('''
    box.h
    box.c

    amf-smf.c
    ../main.c
'''.split())

executable('open5gs-amfsmfd',
    sources : amfsmf_sources,
    c_args : '-DDEFAULT_CONFIG_FILENAME="@0@/amfsmf.yaml"'.format(open5gs_sysconfdir),
    include_directories : srcinc,
    dependencies : [libamf_dep, libsmf_dep],
    install_rpath : libdir,
    install : true)
