# --- Gate Server ---
add_executable(gate_server gate/main.cpp)
target_link_libraries(gate_server PRIVATE AegisEngine)

# --- Logic Server ---
add_executable(logic_server logic/main.cpp)
target_link_libraries(logic_server PRIVATE AegisEngine)