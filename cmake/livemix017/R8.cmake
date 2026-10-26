# R1/R2/R3/R4/R5/R7 already attach their implementations to LiveMixCore.
# R8 integrates those APIs in the existing core sources; no duplicate objects.
if(TARGET LiveMixCore)
    target_compile_features(LiveMixCore INTERFACE cxx_std_17)
endif()
