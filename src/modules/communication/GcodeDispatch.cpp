/*
      This file is part of Smoothie (http://smoothieware.org/). The motion control part is heavily based on Grbl (https://github.com/simen/grbl).
      Smoothie is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.
      Smoothie is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
      You should have received a copy of the GNU General Public License along with Smoothie. If not, see <http://www.gnu.org/licenses/>.
*/

#include <string>
using std::string;
#include "libs/Module.h"
#include "libs/Kernel.h"
#include "utils/Gcode.h"
#include "libs/nuts_bolts.h"
#include "GcodeDispatch.h"
#include "modules/robot/Conveyor.h"
#include "libs/SerialMessage.h"
#include "libs/StreamOutput.h"
#include "libs/FileStream.h"

GcodeDispatch::GcodeDispatch() {}

// Called when the module has just been loaded
void GcodeDispatch::on_module_loaded()
{
    return_error_on_unhandled_gcode = this->kernel->config->value( return_error_on_unhandled_gcode_checksum )->by_default(false)->as_bool();
    this->register_for_event(ON_CONSOLE_LINE_RECEIVED);
    currentline = -1;
    uploading = false;
}

// When a command is received, if it is a Gcode, dispatch it as an object via an event
void GcodeDispatch::on_console_line_received(void *line)
{
    SerialMessage new_message = *static_cast<SerialMessage *>(line);
    string possible_command = new_message.message;

    char first_char = possible_command[0];
    int ln = 0;
    int cs = 0;
    if ( first_char == 'G' || first_char == 'M' || first_char == 'T' || first_char == 'N' ) {

        //Get linenumber
        if ( first_char == 'N' ) {
            Gcode full_line = Gcode(possible_command, new_message.stream);
            ln = (int) full_line.get_value('N');
            int chksum = (int) full_line.get_value('*');

            //Catch message if it is M110: Set Current Line Number
            if ( full_line.has_letter('M') ) {
                if ( ((int) full_line.get_value('M')) == 110 ) {
                    currentline = ln;
                    new_message.stream->printf("ok\r\n");
                    return;
                }
            }

            //Strip checksum value from possible_command
            size_t chkpos = possible_command.find_first_of("*");
            possible_command = possible_command.substr(0, chkpos);
            //Calculate checksum
            if ( chkpos != string::npos ) {
                for (auto c = possible_command.cbegin(); *c != '*' && c != possible_command.cend(); c++)
                    cs = cs ^ *c;
                cs &= 0xff;  // Defensive programming...
                cs -= chksum;
            }
            //Strip line number value from possible_command
            size_t lnsize = possible_command.find_first_not_of("N0123456789.,- ");
            possible_command = possible_command.substr(lnsize);

        } else {
            //Assume checks succeeded
            cs = 0x00;
            ln = currentline + 1;
        }

        //Remove comments
        size_t comment = possible_command.find_first_of(";(");
        if( comment != string::npos ) {
            possible_command = possible_command.substr(0, comment);
        }

        //If checksum passes then process message, else request resend
        int nextline = currentline + 1;
        if( cs == 0x00 && ln == nextline ) {
            if( first_char == 'N' ) {
                currentline = nextline;
            }

            while(possible_command.size() > 0) {
                size_t nextcmd = possible_command.find_first_of("GMT", possible_command.find_first_of("GMT") + 1);
                string single_command;
                if(nextcmd == string::npos) {
                    single_command = possible_command;
                    possible_command = "";
                } else {
                    single_command = possible_command.substr(0, nextcmd);
                    possible_command = possible_command.substr(nextcmd);
                }

                if(!uploading) {
                    //Prepare gcode for dispatch
                    Gcode *gcode = new Gcode(single_command, new_message.stream);
                    gcode->prepare_cached_values();

                    if(gcode->has_m) {
                        switch (gcode->m) {
                            case 28: // start upload command
                                delete gcode;

                                this->upload_filename = "/sd/" + single_command.substr(4); // rest of line is filename
                                // open file
                                upload_fd = fopen(this->upload_filename.c_str(), "w");
                                if(upload_fd != NULL) {
                                    this->uploading = true;
                                    new_message.stream->printf("Writing to file: %s\r\n", this->upload_filename.c_str());
                                } else {
                                    new_message.stream->printf("open failed, File: %s.\r\n", this->upload_filename.c_str());
                                }
                                //printf("Start Uploading file: %s, %p\n", upload_filename.c_str(), upload_fd);
                                continue;

                            case 500: // M500 save volatile settings to config-override
                                // delete the existing file
                                remove(kernel->config_override_filename());
                                // replace stream with one that writes to config-override file
                                gcode->stream = new FileStream(kernel->config_override_filename());
                                // dispatch the M500 here so we can free up the stream when done
                                this->kernel->call_event(ON_GCODE_RECEIVED, gcode );
                                delete gcode->stream;
                                delete gcode;
                                new_message.stream->printf("Settings Stored to %s\r\nok\r\n", kernel->config_override_filename());
                                continue;

                            case 501: // M501 deletes config-override so everything defaults to what is in config
                                remove(kernel->config_override_filename());
                                new_message.stream->printf("config override file deleted %s, reboot needed\r\nok\r\n", kernel->config_override_filename());
                                delete gcode;
                                continue;

                            case 503: { // M503 display live settings and indicates if there is an override file
                                FILE *fd = fopen(kernel->config_override_filename(), "r");
                                if(fd != NULL) {
                                    fclose(fd);
                                    new_message.stream->printf("; config override present: %s\n",  kernel->config_override_filename());

                                } else {
                                    new_message.stream->printf("; No config override\n");
                                }
                                break; // fal through to process by modules
                            }
                        }
                    }

                    //printf("dispatch %p: '%s' G%d M%d...", gcode, gcode->command.c_str(), gcode->g, gcode->m);
                    //Dispatch message!
                    this->kernel->call_event(ON_GCODE_RECEIVED, gcode );
                    if(gcode->add_nl)
                        new_message.stream->printf("\r\n");

                    if( return_error_on_unhandled_gcode == true && gcode->accepted_by_module == false)
                        new_message.stream->printf("ok (command unclaimed)\r\n");
                    else if(!gcode->txt_after_ok.empty()) {
                        new_message.stream->printf("ok %s\r\n", gcode->txt_after_ok.c_str());
                        gcode->txt_after_ok.clear();
                    } else
                        new_message.stream->printf("ok\r\n");

                    delete gcode;

                } else {
                    // we are uploading a file so save it
                    if(single_command.substr(0, 3) == "M29") {
                        // done uploading, close file
                        fclose(upload_fd);
                        upload_fd = NULL;
                        uploading = false;
                        upload_filename.clear();
                        new_message.stream->printf("Done saving file.\r\n");
                        continue;
                    }

                    if(upload_fd == NULL) {
                        // error detected writing to file so discard everything until it stops
                        new_message.stream->printf("ok\r\n");
                        continue;
                    }

                    single_command.append("\n");
                    static int cnt = 0;
                    if(fwrite(single_command.c_str(), 1, single_command.size(), upload_fd) != single_command.size()) {
                        // error writing to file
                        new_message.stream->printf("Error:error writing to file.\r\n");
                        fclose(upload_fd);
                        upload_fd = NULL;
                        continue;

                    } else {
                        cnt += single_command.size();
                        if (cnt > 400) {
                            // HACK ALERT to get around fwrite corruption close and re open for append
                            fclose(upload_fd);
                            upload_fd = fopen(upload_filename.c_str(), "a");
                            cnt = 0;
                        }
                        new_message.stream->printf("ok\r\n");
                        //printf("uploading file write ok\n");
                    }
                }
            }

        } else {
            //Request resend
            new_message.stream->printf("rs N%d\r\n", nextline);
        }

        // Ignore comments and blank lines
    } else if ( first_char == ';' || first_char == '(' || first_char == ' ' || first_char == '\n' || first_char == '\r' ) {
        new_message.stream->printf("ok\r\n");
    }
}

