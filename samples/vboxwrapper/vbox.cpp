// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2010 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

#ifdef _WIN32
#include "boinc_win.h"
#include "win_util.h"
#else
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include <string>
#include <unistd.h>
#endif

using std::string;

#if defined(_MSC_VER)
#define getcwd      _getcwd
#endif

#include "diagnostics.h"
#include "filesys.h"
#include "parse.h"
#include "str_util.h"
#include "str_replace.h"
#include "util.h"
#include "error_numbers.h"
#include "procinfo.h"
#include "boinc_api.h"
#include "vbox.h"

VBOX_VM::VBOX_VM() {
    os_name.clear();
    memory_size_mb.clear();
    image_filename.clear();
    suspended = false;
    network_suspended = false;
    enable_network = false;
    enable_shared_directory = false;
    register_only = false;
}

int VBOX_VM::run() {
    int retval;

    retval = initialize();
    if (retval) return retval;

    if (!is_registered()) {
        if (is_hdd_registered()) {
            // Handle the case where a previous instance of the same projects VM
            // was already initialized for the current slot directory but aborted
            // while the task was suspended and unloaded from memory.
            retval = deregister_stale_vm();
            if (retval) return retval;
        }
        retval = register_vm();
        if (retval) return retval;
    }

    // The user has requested that we exit after registering the VM, so return an
    // error to stop further processing.
    if (register_only) return ERR_FOPEN;

    retval = start();
    if (retval) return retval;

    // Give time enough for external processes to begin the VM boot process
    boinc_sleep(1.0);

    return 0;
}

void VBOX_VM::poll() {
    return;
}

void VBOX_VM::cleanup() {
    stop();
    deregister_vm();

    // Give time enough for external processes to finish the cleanup process
    boinc_sleep(5.0);
}

// Execute the vbox manage application and copy the output to the buffer.
//
int VBOX_VM::vbm_popen(string& arguments, string& output, const char* item) {
    char buf[256];
    string command;
    int retval = 0;

    // Initialize command line
    command = "VBoxManage -q " + arguments;

#ifdef _WIN32

    STARTUPINFO si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;
    HANDLE hReadPipe, hWritePipe;
    void* pBuf;
    DWORD dwCount;
    unsigned long ulExitCode = 0;

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    memset(&sa, 0, sizeof(sa));
    memset(&sd, 0, sizeof(sd));

    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, true, NULL, false);

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = &sd;

    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, NULL)) {
        fprintf(
            stderr,
            "%s CreatePipe failed! (%d).\n",
            boinc_msg_prefix(buf, sizeof(buf)),
            GetLastError()
        );
        goto CLEANUP;
    }
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.cb = sizeof(STARTUPINFO);
    si.dwFlags |= STARTF_FORCEOFFFEEDBACK | STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = NULL;

    // Execute command
    if (!CreateProcess(NULL, (LPTSTR)command.c_str(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        fprintf(
            stderr,
            "%s CreateProcess failed! (%d).\n",
            boinc_msg_prefix(buf, sizeof(buf)),
            GetLastError()
        );
        goto CLEANUP;
    }

    // Wait until process has completed
    while(1) {
        GetExitCodeProcess(pi.hProcess, &ulExitCode);

        // Copy stdout/stderr to output buffer, handle in the loop so that we can
        // copy the pipe as it is populated and prevent the child process from blocking
        // in case the output is bigger than pipe buffer.
        PeekNamedPipe(hReadPipe, NULL, NULL, NULL, &dwCount, NULL);
        if (dwCount) {
            pBuf = malloc(dwCount+1);
            memset(pBuf, 0, dwCount+1);

            if (ReadFile(hReadPipe, pBuf, dwCount, &dwCount, NULL)) {
                output += (char*)pBuf;
            }

            free(pBuf);
        }

        if (ulExitCode != STILL_ACTIVE) break;
        Sleep(100);
    }

CLEANUP:
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    if (hReadPipe) CloseHandle(hReadPipe);
    if (hWritePipe) CloseHandle(hWritePipe);

    if ((ulExitCode != 0) || (!pi.hProcess)) {
        retval = ERR_FOPEN;
    }
#else

    FILE* fp;

    // redirect stderr to stdout for the child process so we can trap it with popen.
    string modified_command = command + " 2>&1";

    // Execute command
    fp = popen(modified_command.c_str(), "r");
    if (fp == NULL){
        fprintf(
            stderr,
            "%s vbm_popen popen failed! errno = %d\n",
            boinc_msg_prefix(buf, sizeof(buf)),
            errno
        );
        retval = ERR_FOPEN;
    } else {
        // Copy output to buffer
        while (fgets(buf, 256, fp)) {
            output += buf;
        }

        // Close stream
        pclose(fp);
        retval = 0;
    }

#endif
    if (retval) {
        fprintf(
            stderr,
            "%s Error in %s for VM: %d\nCommand:\n%s\nOutput:\n%s\n",
            boinc_msg_prefix(buf, sizeof(buf)),
            item,
            retval,
            command.c_str(),
            output.c_str()
        );
    }
    return retval;
}

// Returns the current directory in which the executable resides.
//
int VBOX_VM::get_slot_directory( string& dir ) {
    char slot_dir[256];

    getcwd(slot_dir, sizeof(slot_dir));
    dir = slot_dir;

    if (!dir.empty()) {
        return 1;
    }
    return 0;
}

bool VBOX_VM::is_registered() {
    string command;
    string output;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--machinereadable ";

    if (vbm_popen(command, output, "registration") == 0) {
        if (output.find("VBOX_E_OBJECT_NOT_FOUND") == string::npos) {
            // Error message not found in text
            return true;
        }
    }
    return false;
}

bool VBOX_VM::is_hdd_registered() {
    string command;
    string output;
    string virtual_machine_root_dir;

    get_slot_directory(virtual_machine_root_dir);

    command = "showhdinfo \"" + virtual_machine_root_dir + "/" + image_filename + "\" ";

    if (vbm_popen(command, output, "hdd registration") == 0) {
        if ((output.find("VBOX_E_FILE_ERROR") == string::npos) && (output.find("VBOX_E_OBJECT_NOT_FOUND") == string::npos)) {
            // Error message not found in text
            return true;
        }
    }
    return false;
}

bool VBOX_VM::is_running() {
    string command;
    string output;
    string vmstate;
    size_t vmstate_start;
    size_t vmstate_end;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--machinereadable ";

    if (vbm_popen(command, output, "VM state") == 0) {
        vmstate_start = output.find("VMState=\"");
        if (vmstate_start != string::npos) {
            vmstate_start += 9;
            vmstate_end = output.find("\"", vmstate_start);
            vmstate = output.substr(vmstate_start, vmstate_end - vmstate_start);

            // VirtualBox Documentation suggests that that a VM is running when its
            // machine state is between MachineState_FirstOnline and MachineState_LastOnline
            // which as of this writing is 5 and 17.
            //
            // VboxManage's source shows more than that though:
            // see: http://www.virtualbox.org/browser/trunk/src/VBox/Frontends/VBoxManage/VBoxManageInfo.cpp
            //
            // So for now, go with what VboxManage is reporting.
            //
            if (vmstate == "running") return true;
            if (vmstate == "paused") return true;
            if (vmstate == "gurumeditation") return true;
            if (vmstate == "livesnapshotting") return true;
            if (vmstate == "teleporting") return true;
            if (vmstate == "starting") return true;
            if (vmstate == "stopping") return true;
            if (vmstate == "saving") return true;
            if (vmstate == "restoring") return true;
            if (vmstate == "teleportingpausedvm") return true;
            if (vmstate == "teleportingin") return true;
            if (vmstate == "restoringsnapshot") return true;
            if (vmstate == "deletingsnapshot") return true;
            if (vmstate == "deletingsnapshotlive") return true;
        }
    }
    return false;
}

int VBOX_VM::get_install_directory(string& virtualbox_install_directory ) {
#ifdef _WIN32
    LONG    lReturnValue;
    HKEY    hkSetupHive;
    LPTSTR  lpszRegistryValue = NULL;
    DWORD   dwSize = 0;

    // change the current directory to the boinc data directory if it exists
    lReturnValue = RegOpenKeyEx(
        HKEY_LOCAL_MACHINE, 
        _T("SOFTWARE\\Oracle\\VirtualBox"),  
        0, 
        KEY_READ,
        &hkSetupHive
    );
    if (lReturnValue == ERROR_SUCCESS) {
        // How large does our buffer need to be?
        lReturnValue = RegQueryValueEx(
            hkSetupHive,
            _T("InstallDir"),
            NULL,
            NULL,
            NULL,
            &dwSize
        );
        if (lReturnValue != ERROR_FILE_NOT_FOUND) {
            // Allocate the buffer space.
            lpszRegistryValue = (LPTSTR) malloc(dwSize);
            (*lpszRegistryValue) = NULL;

            // Now get the data
            lReturnValue = RegQueryValueEx( 
                hkSetupHive,
                _T("InstallDir"),
                NULL,
                NULL,
                (LPBYTE)lpszRegistryValue,
                &dwSize
            );

            virtualbox_install_directory = lpszRegistryValue;
        }
    }

    if (hkSetupHive) RegCloseKey(hkSetupHive);
    if (lpszRegistryValue) free(lpszRegistryValue);
#else
    virtualbox_install_directory = "";
#endif
    return 0;
}

int VBOX_VM::initialize() {
    string virtualbox_install_directory;
    string old_path;
    string new_path;
    string virtualbox_user_home;
    APP_INIT_DATA aid;
    char buf[256];

    boinc_get_init_data_p(&aid);
    get_install_directory(virtualbox_install_directory);

    // Prep the environment so we can execute the vboxmanage application
    //
    // TODO: Fix for non-Windows environments if we ever find another platform
    // where vboxmanage is not already in the search path
#ifdef _WIN32
    if (!virtualbox_install_directory.empty())
    {
        old_path = getenv("PATH");
        new_path = virtualbox_install_directory + ";" + old_path;

        if (!SetEnvironmentVariable("PATH", const_cast<char*>(new_path.c_str()))) {
            fprintf(
                stderr,
                "%s Failed to modify the search path.\n",
                boinc_msg_prefix(buf, sizeof(buf))
            );
        }
    }
#endif

    // Set the location in which the VirtualBox Configuration files can be
    // stored for this instance.
    //
    if (aid.using_sandbox) {
        virtualbox_user_home = aid.project_dir;
        virtualbox_user_home += "/../virtualbox";

        if (!boinc_file_exists(virtualbox_user_home.c_str())) boinc_mkdir(virtualbox_user_home.c_str());

#ifdef _WIN32
        if (!SetEnvironmentVariable("VBOX_USER_HOME", const_cast<char*>(virtualbox_user_home.c_str()))) {
            fprintf(
                stderr,
                "%s Failed to modify the search path.\n",
                boinc_msg_prefix(buf, sizeof(buf))
            );
        }
#else
        // putenv does not copy its input buffer, so we must use setenv
        if (setenv("VBOX_USER_HOME", const_cast<char*>(virtualbox_user_home.c_str()), 1)) {
            fprintf(
                stderr,
                "%s Failed to modify the VBOX_USER_HOME path.\n",
                boinc_msg_prefix(buf, sizeof(buf))
            );
        }
#endif
    }

    return 0;
}

int VBOX_VM::register_vm() {
    string command;
    string output;
    string virtual_machine_slot_directory;
    char buf[256];
    int retval;

    get_slot_directory(virtual_machine_slot_directory);

    fprintf(
        stderr,
        "%s Registering virtual machine. (%s) \n",
        boinc_msg_prefix(buf, sizeof(buf)),
        vm_name.c_str()
    );


    // Create and register the VM
    //
    fprintf(
        stderr,
        "%s Registering virtual machine with VirtualBox.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "createvm ";
    command += "--name \"" + vm_name + "\" ";
    command += "--basefolder \"" + virtual_machine_slot_directory + "\" ";
    command += "--ostype \"" + os_name + "\" ";
    command += "--register";
    
    retval = vbm_popen(command, output, "register");
    if (retval) return retval;

    // Tweak the VM from it's default configuration
    //
    fprintf(
        stderr,
        "%s Modifying virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--memory " + memory_size_mb + " ";
    command += "--acpi on ";
    command += "--ioapic on ";
    command += "--boot1 disk ";
    command += "--boot2 none ";
    command += "--boot3 none ";
    command += "--boot4 none ";
    command += "--nic1 nat ";
    command += "--natdnsproxy1 on ";
    command += "--cableconnected1 off ";

    retval = vbm_popen(command, output, "modify");
    if (retval) return retval;

    // Add storage controller to VM
    //
    fprintf(
        stderr,
        "%s Adding storage controller to virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "storagectl \"" + vm_name + "\" ";
    command += "--name \"IDE Controller\" ";
    command += "--add ide ";
    command += "--controller PIIX4 ";

    retval = vbm_popen(command, output, "add storage controller");
    if (retval) return retval;

    // Adding virtual hard drive to VM
    //
    fprintf(
        stderr,
        "%s Adding virtual disk drive to virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "storageattach \"" + vm_name + "\" ";
    command += "--storagectl \"IDE Controller\" ";
    command += "--port 0 ";
    command += "--device 0 ";
    command += "--type hdd ";
    command += "--medium \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";

    retval = vbm_popen(command, output, "storage attach");
    if (retval) return retval;

    // Enable the network adapter if a network connection is required.
    //
    if (enable_network) {
        set_network_access(true);
    }

    // Enable the shared folder if a shared folder is specified.
    //
    if (enable_shared_directory) {
        fprintf(
            stderr,
            "%s Enabling shared directory for virtual machine.\n",
            boinc_msg_prefix(buf, sizeof(buf))
        );
        command  = "sharedfolder add \"" + vm_name + "\" ";
        command += "--name \"shared\" ";
        command += "--hostpath \"" + virtual_machine_slot_directory + "/shared\"";

        retval = vbm_popen(command, output, "enable shared dir");
        if (retval) return retval;
    }
    return 0;
}

int VBOX_VM::deregister_vm() {
    string command;
    string output;
    string virtual_machine_slot_directory;
    char buf[256];
    int retval;

    get_slot_directory(virtual_machine_slot_directory);

    fprintf(
        stderr,
        "%s Deregistering virtual machine. (%s)\n",
        boinc_msg_prefix(buf, sizeof(buf)),
        vm_name.c_str()
    );


    // First step in deregistering a VM is to delete its storage controller
    //
    fprintf(
        stderr,
        "%s Removing storage controller from virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "storagectl \"" + vm_name + "\" ";
    command += "--name \"IDE Controller\" ";
    command += "--remove ";

    retval = vbm_popen(command, output, "deregister");
    if (retval) return retval;

    // Next delete VM
    //
    fprintf(
        stderr,
        "%s Removing virtual machine from VirtualBox.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "unregistervm \"" + vm_name + "\" ";
    command += "--delete ";

    retval = vbm_popen(command, output, "delete VM");
    if (retval) return retval;

    // Lastly delete medium from Virtual Box Media Registry
    //
    fprintf(
        stderr,
        "%s Removing virtual disk drive from VirtualBox.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command  = "closemedium disk \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";

    retval = vbm_popen(command, output, "remove virtual disk");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::deregister_stale_vm() {
    string command;
    string output;
    string virtual_machine_slot_directory;
    size_t uuid_start;
    size_t uuid_end;
    int retval;

    get_slot_directory(virtual_machine_slot_directory);

    // We need to determine what the name or uuid is of the previous VM which owns
    // this virtual disk
    //
    command  = "showhdinfo \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";

    retval = vbm_popen(command, output, "get HDD info");
    if (retval) return retval;

    // Output should look a little like this:
    //   UUID:                 c119acaf-636c-41f6-86c9-38e639a31339
    //   Accessible:           yes
    //   Logical size:         10240 MBytes
    //   Current size on disk: 0 MBytes
    //   Type:                 normal (base)
    //   Storage format:       VDI
    //   Format variant:       dynamic default
    //   In use by VMs:        test2 (UUID: 000ab2be-1254-4c6a-9fdc-1536a478f601)
    //   Location:             C:\Users\romw\VirtualBox VMs\test2\test2.vdi
    //
    uuid_start = output.find("(UUID: ");
    if (uuid_start != string::npos) {
        // We can parse the virtual machine ID from the output
        uuid_start += 7;
        uuid_end = output.find(")", uuid_start);
        vm_name = output.substr(uuid_start, uuid_end - uuid_start);

        // Deregister stale VM by UUID
        return deregister_vm();
    } else {
        // Did the user delete the VM in VirtualBox and not the medium?  If so,
        // just remove the medium.
        command  = "closemedium disk \"" + virtual_machine_slot_directory + "/" + image_filename + "\" ";
        retval = vbm_popen(command, output, "remove medium");
    }
    return 0;
}

int VBOX_VM::start() {
    string command;
    string output;
    char buf[256];
    int retval;

    fprintf(
        stderr,
        "%s Starting virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    command = "startvm \"" + vm_name + "\" --type headless";
    retval = vbm_popen(command, output, "start VM");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::stop() {
    string command;
    string output;
    char buf[256];
    int retval;

    fprintf(
        stderr,
        "%s Stopping virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    if (is_running()) {
        command = "controlvm \"" + vm_name + "\" savestate";
        retval = vbm_popen(command, output, "stop VM");
        if (retval) return retval;
    } else {
        fprintf(
            stderr,
            "%s Virtual machine is already in a stopped state.\n",
            boinc_msg_prefix(buf, sizeof(buf))
        );
    }
    return 0;
}

int VBOX_VM::pause() {
    string command;
    string output;
    int retval;

    command = "controlvm \"" + vm_name + "\" pause";
    retval = vbm_popen(command, output, "pause VM");
    if (retval) return retval;
    suspended = true;
    return 0;
}

int VBOX_VM::resume() {
    string command;
    string output;
    int retval;

    command = "controlvm \"" + vm_name + "\" resume";
    retval = vbm_popen(command, output, "resume VM");
    if (retval) return retval;
    suspended = false;
    return 0;
}

// Enable the network adapter if a network connection is required.
// NOTE: Network access should never be allowed if the code running in a 
//   shared directory or the VM itself is NOT signed.  Doing so opens up 
//   the network behind the firewall to attack.
//
int VBOX_VM::set_network_access(bool enabled) {
    string command;
    string output;
    char buf[256];
    int retval;

    network_suspended = !enabled;

    if (enabled) {
        fprintf(
            stderr,
            "%s Enabling network access for virtual machine.\n",
            boinc_msg_prefix(buf, sizeof(buf))
        );
        command  = "modifyvm \"" + vm_name + "\" ";
        command += "--cableconnected1 on ";

        retval = vbm_popen(command, output, "enable network");
        if (retval) return retval;
    } else {
        fprintf(
            stderr,
            "%s Disabling network access for virtual machine.\n",
            boinc_msg_prefix(buf, sizeof(buf))
        );
        command  = "modifyvm \"" + vm_name + "\" ";
        command += "--cableconnected1 off ";

        retval = vbm_popen(command, output, "disable network");
        if (retval) return retval;
    }
    return 0;
}

int VBOX_VM::set_cpu_usage_fraction(double x) {
    string command;
    string output;
    char buf[256];
    int retval;

    // the arg to modifyvm is percentage
    //
    fprintf(
        stderr,
        "%s Setting cpu throttle for virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    sprintf(buf, "%d", (int)(x*100.));
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--cpuexecutioncap ";
    command += buf;
    command += " ";

    retval = vbm_popen(command, output, "CPU throttle");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::set_network_max_bytes_sec(double x) {
    string command;
    string output;
    char buf[256];
    int retval;


    // the argument to modifyvm is in Kbps
    //
    fprintf(
        stderr,
        "%s Setting network throttle for virtual machine.\n",
        boinc_msg_prefix(buf, sizeof(buf))
    );
    sprintf(buf, "%d", (int)(x*8./1000.));
    command  = "modifyvm \"" + vm_name + "\" ";
    command += "--nicspeed1 ";
    command += buf;
    command += " ";

    retval = vbm_popen(command, output, "network throttle");
    if (retval) return retval;
    return 0;
}

int VBOX_VM::get_process_id(int& process_id) {
    string command;
    string output;
    string pid;
    size_t pid_start;
    size_t pid_end;
    char buf[256];
    int retval;

    command  = "showvminfo \"" + vm_name + "\" ";
    command += "--log 0 ";

    retval = vbm_popen(command, output, "get process ID");
    if (retval) return retval;

    // Output should look like this:
    // VirtualBox 4.1.0 r73009 win.amd64 (Jul 19 2011 13:05:53) release log
    // 00:00:06.008 Log opened 2011-09-01T23:00:59.829170900Z
    // 00:00:06.008 OS Product: Windows 7
    // 00:00:06.009 OS Release: 6.1.7601
    // 00:00:06.009 OS Service Pack: 1
    // 00:00:06.015 Host RAM: 4094MB RAM, available: 876MB
    // 00:00:06.015 Executable: C:\Program Files\Oracle\VirtualBox\VirtualBox.exe
    // 00:00:06.015 Process ID: 6128
    // 00:00:06.015 Package type: WINDOWS_64BITS_GENERIC
    // 00:00:06.015 Installed Extension Packs:
    // 00:00:06.015   None installed!
    //
    pid_start = output.find("Process ID: ");
    if (pid_start == string::npos) {
        fprintf(stderr, "%s couldn't find 'Process ID: ' in %s\n", boinc_msg_prefix(buf, sizeof(buf)), output.c_str());
        return ERR_NOT_FOUND;
    }
    pid_start += 12;
    pid_end = output.find("\n", pid_start);
    pid = output.substr(pid_start, pid_end - pid_start);
    if (pid.size() <= 0) {
        fprintf(stderr, "%s no PID: location %d length %d\n",
            boinc_msg_prefix(buf, sizeof(buf)), (int)pid_start, (int)(pid_end - pid_start)
        );
        return ERR_NOT_FOUND;
    }
    process_id = atol(pid.c_str());
    if (process_id) {
        fprintf(stderr, "%s Virtual Machine PID %d Detected\n",
            boinc_msg_prefix(buf, sizeof(buf)), process_id
        );
    }
    return 0;
}

int VBOX_VM::get_network_bytes_received(double& received) {
    string command;
    string output;
    string counter_value;
    size_t counter_start;
    size_t counter_end;
    int retval;

    command  = "debugvm \"" + vm_name + "\" ";
    command += "statistics --pattern \"/Devices/*/ReceiveBytes\" ";

    retval = vbm_popen(command, output, "get bytes received");
    if (retval) return retval;

    // Output should look like this:
    // <?xml version="1.0" encoding="UTF-8" standalone="no"?>
    // <Statistics>
    // <Counter c="9423150" unit="bytes" name="/Devices/PCNet0/ReceiveBytes"/>
    // <Counter c="256" unit="bytes" name="/Devices/PCNet1/ReceiveBytes"/>
    // </Statistics>

    // add up the counter(s)
    //
    received = 0;
    counter_start = output.find("c=\"");
    while (counter_start != string::npos) {
        counter_start += 3;
        counter_end = output.find("\"", counter_start);
        counter_value = output.substr(counter_start, counter_end - counter_start);
        received += atof(counter_value.c_str());
        counter_start = output.find("\n", counter_start);
    }

    return 0;
}

int VBOX_VM::get_network_bytes_sent(double& sent) {
    string command;
    string output;
    string counter_value;
    size_t counter_start;
    size_t counter_end;
    int retval;

    command  = "debugvm \"" + vm_name + "\" ";
    command += "statistics --pattern \"/Devices/*/TransmitBytes\" ";

    retval = vbm_popen(command, output, "get bytes sent");
    if (retval) return retval;

    // Output should look like this:
    // <?xml version="1.0" encoding="UTF-8" standalone="no"?>
    // <Statistics>
    // <Counter c="397229" unit="bytes" name="/Devices/PCNet0/TransmitBytes"/>
    // <Counter c="256" unit="bytes" name="/Devices/PCNet1/TransmitBytes"/>
    // </Statistics>

    // add up the counter(s)
    //
    sent = 0;
    counter_start = output.find("c=\"");
    while (counter_start != string::npos) {
        counter_start += 3;
        counter_end = output.find("\"", counter_start);
        counter_value = output.substr(counter_start, counter_end - counter_start);
        sent += atof(counter_value.c_str());
        counter_start = output.find("\n", counter_start);
    }
    return 0;
}
