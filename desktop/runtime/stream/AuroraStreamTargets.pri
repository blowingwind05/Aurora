# SPDX-License-Identifier: GPL-3.0-only
# The app and private engine archive reuse the same Qt and upstream dependency settings.
CONFIG(debug, debug|release) {
    aurora_engine_dir = $$OUT_PWD/engine/debug
} else {
    aurora_engine_dir = $$OUT_PWD/engine/release
}

aurora_stream_engine {
    TEMPLATE = lib
    TARGET = AuroraMoonlightEngine
    CONFIG += staticlib
    CONFIG -= app_bundle lib_bundle qtquickcompiler
    DESTDIR = $$aurora_engine_dir
    OBJECTS_DIR = $$aurora_engine_dir/obj
    MOC_DIR = $$aurora_engine_dir/moc
    RESOURCES =
    TRANSLATIONS =
    RC_ICONS =
    QMAKE_BUNDLE_DATA =
} else {
    SOURCES = AuroraStreamMain.cpp AuroraStreamInvocation.cpp
    HEADERS = AuroraStreamEntry.h
    OBJECTIVE_SOURCES =

    win32: aurora_engine_archive = $$aurora_engine_dir/AuroraMoonlightEngine.lib
    else: aurora_engine_archive = $$aurora_engine_dir/libAuroraMoonlightEngine.a
    PRE_TARGETDEPS += $$quote($$aurora_engine_archive)
    LIBS = $$quote($$aurora_engine_archive) $$LIBS
}
