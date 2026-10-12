# Copies any .o2r archives generated in the build tree (soh.o2r from GenerateSohOtr; oot.o2r /
# oot-mq.o2r from ExtractAssets) next to the built executable. On single-config generators
# (Ninja/Makefiles, used on Linux/macOS) they already land there. On multi-config generators
# (Visual Studio) the executable's directory differs, so without this the game can't find its
# own assets after a plain IDE/MSBuild build (only `cpack` packaging copied them).
# Invoked as: cmake -DSRC_DIR=... -DDST_DIR=... -P CopyGameArchives.cmake
file(GLOB O2R_ARCHIVES "${SRC_DIR}/*.o2r")
if(O2R_ARCHIVES)
    file(COPY ${O2R_ARCHIVES} DESTINATION "${DST_DIR}")
endif()
