idf_component_register(
    SRCS "dsp_core.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES espressif__esp-dsp
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
