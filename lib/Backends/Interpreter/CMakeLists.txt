add_library(Interpreter
              Interpreter.cpp
              InterpreterFunction.cpp
              InterpreterNodes.cpp)
target_link_libraries(Interpreter
                      PRIVATE
                        Base
                        Graph
                        CodeGen
                        BackendUtils
                        IR
                        Optimizer
                        QuantizationBase)

add_library(InterpreterDeviceManager
              InterpreterDeviceManager.cpp)
target_link_libraries(InterpreterDeviceManager
                      PRIVATE
                        Backends
                        BackendUtils
                        Base
                        CodeGen
                        Graph
                        Interpreter
                        IR
                        Optimizer)
