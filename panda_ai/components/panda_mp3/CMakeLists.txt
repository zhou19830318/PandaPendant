idf_component_register(
    SRCS "panda_mp3.c"
    INCLUDE_DIRS "include"
    REQUIRES panda_board panda_audio chmorgan__esp-libhelix-mp3
)
