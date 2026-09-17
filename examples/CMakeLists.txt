add_executable(thumbnail thumbnail.cpp)
target_link_libraries(thumbnail PRIVATE stills::stills)
stills_apply_warnings(thumbnail)

add_executable(contact_sheet contact_sheet.cpp)
target_link_libraries(contact_sheet PRIVATE stills::stills)
stills_apply_warnings(contact_sheet)
