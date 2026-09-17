 # Strict warning set applied to *our* executables only (never to the INTERFACE library target,
# so consumers are not forced into our flags). GCC/Clang only: detail/config.hpp hard-errors
# without __int128, which MSVC does not have, so an MSVC branch here could never run.
function(stills_apply_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
      -Wall -Wextra -Wpedantic
      -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wcast-align
      -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference
      -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough>
    $<$<CXX_COMPILER_ID:GNU>:
      -Wduplicated-cond -Wlogical-op -Wmisleading-indentation>)
  if(STILLS_WERROR)
    set_property(TARGET ${target} PROPERTY COMPILE_WARNING_AS_ERROR ON)
  endif()
endfunction()
