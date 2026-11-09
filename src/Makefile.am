# Gens/GS build system.
# src/: Main source code directory.

AUTOMAKE_OPTIONS = foreign subdir-objects

if GENS_CORE_C
CPU68K_SUBDIR = musashi
else
CPU68K_SUBDIR = starscream
endif

SUBDIRS = extlib $(CPU68K_SUBDIR) mdZ80 libgsft cbob gens mdp
DIST_SUBDIRS = extlib starscream musashi mdZ80 libgsft cbob gens mdp
