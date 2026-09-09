# Included after project() by the samples that verify the platform's chain.

# The platform serves GTS Root R4 cross-signed by GlobalSign, so a device needs
# only the self-signed root itself; the cross-sign here would be a second copy
# of an anchor the server already sends, expiring nine years before it does.
generate_inc_file_for_target(
  app ${CMAKE_CURRENT_LIST_DIR}/cert/GTS_Root_R4.crt
  ${ZEPHYR_BINARY_DIR}/include/generated/GTS_Root_R4.crt.hex
)
