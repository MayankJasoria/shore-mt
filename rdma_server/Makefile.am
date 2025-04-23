## Process this file with automake to produce Makefile.in
# This builds the rdma_server executable

include $(top_srcdir)/Makefile.generic

# Define the executable target
bin_PROGRAMS = rdma_server

# List ONLY the source files (.c, .cpp, .S, etc.) that need to be compiled.
# Headers are NOT listed in _SOURCES.
rdma_server_SOURCES = \
	rdma_server.c \
	impl/file_hash_table.c \
	impl/fsync_queue.c \
	impl/queue.c \
	impl/write_queue.c \
	impl/read_queue.c

# Add include paths for rlibv2 headers and the local 'extras' directory.
# Use $(srcdir) for paths relative to the current source directory.
# AM_CPPFLAGS are used by both C and C++ compilers for include paths and defines.
rdma_server_CPPFLAGS = $(AM_CPPFLAGS) \
	-I$(top_srcdir)/rlibv2/core \
	-I$(top_srcdir)/rlibv2/c_wrappers \
	-I$(srcdir)/extras

# General linker flags (if any are needed, often handled by default or configure variables)
# rdma_server_LDFLAGS = # Remove or keep other necessary flags if any

# Add libraries needed by the executable for linking.
# Use _LIBADD for specifying library dependencies.
# Ensure $(IBVERBS_LIBS), $(GFLAGS_LIBS), $(PTHREAD_LIBS) are defined in configure.ac and passed down.
# Use $(top_builddir) to specify the path to libraries built within the project.
rdma_server_LDADD = \
    $(top_builddir)/rlibv2/librlibv2.a \
    $(LOCALLDADD) \
    $(GFLAGS_LIBS) \
    -libverbs \
    -lpthread \
    -lstdc++ \
    $(LIBS)

# Headers in 'extras' are internal and should NOT be installed publicly.
# Do NOT list them in include_HEADERS.
# include_HEADERS = # No public headers to install from this directory

# Clean up any generated files specific to this executable (if any)
# MOSTLYCLEANFILES =
# CLEANFILES =