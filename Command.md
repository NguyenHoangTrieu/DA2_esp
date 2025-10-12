Here is a summary table of common `idf.py` commands used in ESP-IDF development, including their purposes and options:[2][5][6]

| Command                | Usage                                  | Description / Options                                                                                                       |
|------------------------|----------------------------------------|----------------------------------------------------------------------------------------------------------------------------|
| create-project         | idf.py create-project <name>           | Creates a new ESP-IDF project (`--path` for folder)                                                                        |
| create-component       | idf.py create-component <name>         | Creates a minimal component (`-C` for directory)                                                                           |
| set-target             | idf.py set-target <target>             | Sets the project target chip (clears build, resets sdkconfig)                                                              |
| build                  | idf.py build                           | Builds the project using CMake and Ninja                                                                                   |
| flash                  | idf.py flash                           | Builds (if needed) and flashes to chip (`-p` port, `-b` baudrate, `--extra-args` for esptool.py params)                   |
| menuconfig             | idf.py menuconfig                      | Opens interactive configuration menu                                                                                       |
| monitor                | idf.py monitor                         | Serial monitor for device output (`-p` port)                                                                               |
| clean                  | idf.py clean                           | Removes intermediate build files                                                                                           |
| fullclean              | idf.py fullclean                       | Removes build and configuration files; cleans more deeply than `clean`                                                     |
| reconfigure            | idf.py reconfigure                     | Forces CMake reconfiguration (useful after changing files or variables)                                                    |
| size                   | idf.py size                            | Shows app size summary (output: text/csv/json)                                                                             |
| size-components        | idf.py size-components                 | Shows size per component                                                                                                   |
| size-files             | idf.py size-files                      | Shows size per source file                                                                                                 |
| partition-table        | idf.py partition-table                 | Prints summary information for the partition table                                                                         |
| docs                   | idf.py docs                            | Opens documentation for target chip/version                                                                                |
| python-clean           | idf.py python-clean                    | Removes Python byte code in ESP-IDF root (helpful after Python/ESP-IDF change)                                             |
| uf2                    | idf.py uf2                             | Generates UF2 binary (all-in-one flashable file)                                                                           |
| uf2-app                | idf.py uf2-app                         | Generates UF2 binary for application only                                                                                  |
| add-dependency         | idf.py add-dependency <name>           | Adds a component dependency to the project (`--path` for local path, `--version` for specific version)                    |