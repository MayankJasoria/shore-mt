lib_LIBRARIES = librlibv2.a
librlibv2_a_SOURCES = c_wrappers/impl/rdmaio_config_c.cpp \
                                  c_wrappers/impl/rdmaio_impl_c.cpp \
                                  c_wrappers/impl/rdmaio_lib_c.cpp \
                                  c_wrappers/impl/rdmaio_mem_c.cpp \
                                  c_wrappers/impl/rdmaio_nic_c.cpp \
                                  c_wrappers/impl/rdmaio_rc_c.cpp \
                                  c_wrappers/impl/rdmaio_rc_recv_manager_c.cpp \
                                  c_wrappers/impl/rdmaio_rctrl_c.cpp \
                                  c_wrappers/impl/rdmaio_recv_iter_c.cpp \
                                  c_wrappers/impl/rdmaio_reg_handler_c.cpp

librlibv2_a_CXXFLAGS = $(AM_CXXFLAGS) -Icore -Ic_wrappers
librlibv2_a_LIBADD =