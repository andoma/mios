/* Shared linker section arrays — INCLUDE this inside a rodata/data section */

                . = ALIGN(8);
                _clicmd_array_begin = .;
                KEEP(*(SORT_BY_NAME(clicmd.*)))
                _clicmd_array_end = .;

                . = ALIGN(8);
                _rpcdef_array_begin = .;
                KEEP(*(rpcdef))
                _rpcdef_array_end = .;

                . = ALIGN(8);
                _servicedef_array_begin = .;
                KEEP(*(servicedef))
                _servicedef_array_end = .;

                . = ALIGN(8);
                _httproute_array_begin = .;
                KEEP(*(httproute))
                _httproute_array_end = .;

                . = ALIGN(8);
                _udpinput_array_begin = .;
                KEEP(*(udpinput))
                _udpinput_array_end = .;

                . = ALIGN(8);
                _ghook_array_begin = .;
                KEEP(*(ghook))
                _ghook_array_end = .;

                . = ALIGN(8);
                _copyhandler_array_begin = .;
                KEEP(*(SORT_BY_NAME(copyhandler.*)))
                _copyhandler_array_end = .;

                . = ALIGN(8);
                _driver_array_begin = .;
                KEEP(*(SORT_BY_NAME(driver.*)))
                _driver_array_end = .;
