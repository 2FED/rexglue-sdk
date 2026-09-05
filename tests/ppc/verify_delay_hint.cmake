# Functional state tests also pass if the delay is deleted. Check the real
# generated instruction stream so the original comment-only bug fails here.
file(READ "${GENERATED_FUNCTIONS}" functions)
string(REGEX MATCHALL "// db16cyc[ \t]*[\r\n]+[ \t]*simde_mm_pause\\(\\);" hints "${functions}")
list(LENGTH hints hint_count)
string(REGEX MATCHALL "simde_mm_pause\\(\\);" calls "${functions}")
list(LENGTH calls call_count)
if(NOT hint_count EQUAL 9 OR NOT call_count EQUAL 9)
  message(FATAL_ERROR "Expected 9 exact delay-hint emissions and no ordinary-OR hints; got ${hint_count}/${call_count}")
endif()
message(STATUS "9 exact db16cyc host hints verified")
