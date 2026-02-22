project('gpu-screen-recorder-adwaita', 'c',
    version : '1.0.0',
    default_options : ['warning_level=2', 'c_std=gnu23'],
)

add_project_arguments('-Wshadow', language : 'c')
if get_option('buildtype') == 'debug'
    add_project_arguments('-g3', language : 'c')
elif get_option('buildtype') == 'release'
    add_project_arguments('-DNDEBUG', language : 'c')
endif

src = [
    'src/main.c',
    'src/gsr-window.c',
    'src/gsr-info.c',
    'src/gsr-config.c',
    'src/gsr-config-page.c',
    'src/gsr-stream-page.c',
    'src/gsr-record-page.c',
    'src/gsr-replay-page.c',
    'src/gsr-hotkeys.c',
]

dep = [
    dependency('libadwaita-1', version : '>=1.8'),
]

c_args = [
    '-DGSR_ICONS_PATH="' + join_paths(get_option('prefix'), get_option('datadir'), 'icons') + '"',
    '-DGSR_VERSION="' + meson.project_version() + '"',
]

if get_option('x11')
    src += [
        'src/gsr-x11-hotkeys.c',
        'src/gsr-x11-window-picker.c',
        'src/gsr-shortcut-accel-dialog.c',
    ]
    dep += dependency('x11')
    c_args += '-DHAVE_X11'
endif

if get_option('wayland')
    src += [
        'src/global_shortcuts.c',
    ]
    c_args += '-DHAVE_WAYLAND'
endif

executable('gpu-screen-recorder-adw',
    src,
    dependencies : dep,
    include_directories : include_directories('src'),
    install : true,
    c_args : c_args,
)

install_data(
    files('com.dec05eba.gpu_screen_recorder.desktop'),
    install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'applications'),
)
install_subdir('icons/hicolor', install_dir : join_paths(get_option('prefix'), get_option('datadir'), 'icons'))

gnome = import('gnome')
gnome.post_install(gtk_update_icon_cache : true, update_desktop_database : true)
