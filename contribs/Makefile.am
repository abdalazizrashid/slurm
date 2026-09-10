SUBDIRS = openlava pam perlapi pmi pmi2 seff sgather sjobexit slurm_completion_help torque

if DARWIN_BUILD
SUBDIRS += macos
endif

if HAVE_LUA
SUBDIRS += lua
endif

if LINUX_BUILD
SUBDIRS += nss_slurm pam_slurm_adopt
endif
