// app.cpp - Interactive C++ user-space program for the /dev/mydev driver.
// ========================================================================
//
// This program runs in USER space.  It cannot touch the kernel buffer
// directly; instead it asks the kernel to do work through SYSTEM CALLS:
//
//     open()   -> get a file descriptor (a small int) for /dev/mydev
//     write()  -> kernel runs mydev_write()  : user string -> kernel buffer
//     lseek()  -> kernel runs mydev_llseek() : move the file position
//     read()   -> kernel runs mydev_read()   : kernel buffer -> user memory
//     ioctl()  -> kernel runs mydev_ioctl()  : clear / query size
//     close()  -> kernel runs mydev_release()
//
// Build:  make app        (or simply `make` to build everything)
// Run:    ./app           (after loading the module with ./mydev.sh load)

// ---- C / POSIX headers: the thin wrappers around Linux system calls ----
#include <fcntl.h>      // open() and the O_RDWR flag
#include <unistd.h>     // read(), write(), lseek(), close()
#include <sys/ioctl.h>  // ioctl()

// ---- C++ standard library headers ----
#include <cerrno>        // errno: the error code set by a failed system call
#include <cstddef>       // std::size_t
#include <iostream>      // std::cout, std::cin, std::cerr
#include <string>        // std::string, std::getline, std::stoi
#include <system_error>  // std::system_error: exception carrying an errno
#include <vector>        // std::vector, used as a read buffer

#include "mydev_ioctl.h" // shared contract with the kernel module

// An "anonymous namespace" makes everything inside private to this file.
namespace {

// Path of the device file created by the driver + udev.
constexpr const char* kDevicePath = "/dev/mydev";

// Helper: turn the current errno into a C++ exception with a readable
// message, e.g. "open(/dev/mydev): No such file or directory".
// [[noreturn]] tells the compiler this function never returns normally.
[[noreturn]] void throwErrno(const std::string& what) {
    throw std::system_error(errno, std::generic_category(), what);
}

// ------------------------------------------------------------------------
// class MyDevice
//   Wraps the file descriptor in a C++ object using RAII ("Resource
//   Acquisition Is Initialization"):
//     - the constructor opens the device,
//     - the destructor closes it automatically,
//   so the descriptor can never be leaked, even if an exception is thrown.
// ------------------------------------------------------------------------
class MyDevice {
public:
    // Constructor: open the device for reading AND writing (O_RDWR).
    // On success open() returns a file descriptor (>= 0); on failure -1.
    explicit MyDevice(const std::string& path)
        : fd_(::open(path.c_str(), O_RDWR)) {   // "::" = the global C function
        if (fd_ < 0) {
            throwErrno("open(" + path + ")");
        }
    }

    // Destructor: runs automatically when the object goes out of scope.
    ~MyDevice() {
        if (fd_ >= 0) {
            ::close(fd_);   // kernel calls mydev_release()
        }
    }

    // Forbid copying: two objects owning the same fd would close it twice.
    MyDevice(const MyDevice&) = delete;
    MyDevice& operator=(const MyDevice&) = delete;

    // Store `text` in the kernel buffer, replacing what was there.
    // Returns how many bytes the driver actually accepted (max 1024).
    std::size_t writeString(const std::string& text) {
        // Go back to position 0 so we overwrite from the start.
        rewind();
        // write(fd, pointer to bytes, number of bytes)
        ssize_t n = ::write(fd_, text.data(), text.size());
        if (n < 0) {
            throwErrno("write");
        }
        return static_cast<std::size_t>(n);   // ssize_t -> size_t
    }

    // Read the whole stored contents back from the kernel buffer.
    std::string readAll() {
        rewind();                                  // start reading at 0
        std::string result;                        // collected data
        std::vector<char> chunk(MYDEV_BUFFER_SIZE);// temporary byte buffer

        // read() may return fewer bytes than requested, so loop until it
        // returns 0, which means "end of file" (no more data).
        while (true) {
            ssize_t n = ::read(fd_, chunk.data(), chunk.size());
            if (n < 0) {
                if (errno == EINTR) continue;      // interrupted: retry
                throwErrno("read");
            }
            if (n == 0) break;                     // end of data
            result.append(chunk.data(), static_cast<std::size_t>(n));
        }
        return result;
    }

    // ioctl with no argument: ask the driver to erase the buffer.
    void clear() {
        if (::ioctl(fd_, MYDEV_IOC_CLEAR) < 0) {
            throwErrno("ioctl(MYDEV_IOC_CLEAR)");
        }
    }

    // ioctl that returns a value: we pass the ADDRESS of an int, and the
    // kernel fills it in using put_user().
    int capacity() {
        int value = 0;
        if (::ioctl(fd_, MYDEV_IOC_GET_SIZE, &value) < 0) {
            throwErrno("ioctl(MYDEV_IOC_GET_SIZE)");
        }
        return value;
    }

    // Number of bytes currently stored.
    int used() {
        int value = 0;
        if (::ioctl(fd_, MYDEV_IOC_GET_LEN, &value) < 0) {
            throwErrno("ioctl(MYDEV_IOC_GET_LEN)");
        }
        return value;
    }

    // Expose the raw descriptor number (just to show it to the user).
    int fd() const { return fd_; }

private:
    // Move this open file's position back to byte 0.
    void rewind() {
        if (::lseek(fd_, 0, SEEK_SET) < 0) {
            throwErrno("lseek");
        }
    }

    int fd_;   // the file descriptor returned by open()
};

// Print the menu.
void printMenu() {
    std::cout << "\n========== /dev/mydev menu ==========\n"
              << " 1) Write a string\n"
              << " 2) Read buffer\n"
              << " 3) Clear buffer (ioctl)\n"
              << " 4) Query buffer size (ioctl)\n"
              << " 5) Exit\n"
              << "Choose an option: " << std::flush;
}

}  // namespace

// ------------------------------------------------------------------------
// main: open the device, then loop showing the menu until the user exits.
// ------------------------------------------------------------------------
int main() {
    // Step 1: open the device.  If that fails, explain the likely cause.
    // `dev` lives on the stack, so it is destroyed (closing the fd)
    // automatically when main() returns or an exception escapes.
    try {
        MyDevice dev(kDevicePath);
        std::cout << "Opened " << kDevicePath
                  << " - file descriptor = " << dev.fd() << '\n';

        std::string line;   // holds each line the user types

        // Step 2: menu loop.
        while (true) {
            printMenu();

            // std::getline returns false at end-of-input (Ctrl+D): quit.
            if (!std::getline(std::cin, line)) {
                std::cout << '\n';
                break;
            }

            // Convert the typed text to a number; non-numbers become 0.
            int choice = 0;
            try {
                choice = std::stoi(line);
            } catch (const std::exception&) {
                choice = 0;
            }

            // Each operation is wrapped in its own try block so that one
            // failed system call shows an error but does not end the app.
            try {
                switch (choice) {
                case 1: {
                    std::cout << "Enter text to store: " << std::flush;
                    std::string text;
                    if (!std::getline(std::cin, text)) break;
                    std::size_t written = dev.writeString(text);
                    std::cout << "Wrote " << written << " byte(s) to "
                              << kDevicePath << '\n';
                    if (written < text.size()) {
                        std::cout << "Note: text was longer than the "
                                  << MYDEV_BUFFER_SIZE
                                  << "-byte buffer and was truncated.\n";
                    }
                    break;
                }
                case 2: {
                    std::string data = dev.readAll();
                    if (data.empty()) {
                        std::cout << "Buffer is empty.\n";
                    } else {
                        std::cout << "Read " << data.size()
                                  << " byte(s): \"" << data << "\"\n";
                    }
                    break;
                }
                case 3:
                    dev.clear();
                    std::cout << "Buffer cleared.\n";
                    break;
                case 4:
                    std::cout << "Buffer capacity : " << dev.capacity()
                              << " bytes\n"
                              << "Bytes in use    : " << dev.used()
                              << " bytes\n";
                    break;
                case 5:
                    std::cout << "Goodbye!\n";
                    return 0;   // dev's destructor closes the fd here
                default:
                    std::cout << "Invalid choice, please enter 1-5.\n";
                    break;
                }
            } catch (const std::system_error& e) {
                // e.what() contains the operation name and errno text.
                std::cerr << "Error: " << e.what() << '\n';
            }
        }
    } catch (const std::system_error& e) {
        // Most likely the module is not loaded or permissions are wrong.
        std::cerr << "Error: " << e.what() << '\n'
                  << "Hint: build with `make`, then load the driver with "
                     "`sudo ./mydev.sh load`.\n";
        return 1;   // non-zero exit status = failure
    }
    return 0;
}
