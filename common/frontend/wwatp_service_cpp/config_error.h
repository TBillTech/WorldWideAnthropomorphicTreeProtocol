#pragma once

#include <exception>
#include <string>
#include <sstream>
#include <iostream>

/**
 * ConfigError - A streamable exception class for configuration errors
 * 
 * This class allows building error messages using stream syntax while
 * being throwable as a standard exception. It maintains an internal
 * stringstream to accumulate the error message.
 * 
 * Usage examples:
 *   throw ConfigError() << "Missing backend: " << backend_name;
 *   throw ConfigError() << "Cycle detected: " << node1 << " -> " << node2;
 */
class ConfigError : public std::exception {
public:
    /**
     * Default constructor
     */
    ConfigError() = default;

    /**
     * Constructor with initial message
     * @param message Initial error message
     */
    explicit ConfigError(const std::string& message) : has_errors_(true) {
        stream_ << message;
    }

    /**
     * Copy constructor
     */
    ConfigError(const ConfigError& other) : has_errors_(other.has_errors_) {
        stream_ << other.stream_.str();
    }

    /**
     * Assignment operator
     */
    ConfigError& operator=(const ConfigError& other) {
        if (this != &other) {
            stream_.str("");
            stream_.clear();
            stream_ << other.stream_.str();
            has_errors_ = other.has_errors_;
        }
        return *this;
    }

    /**
     * Stream insertion operator for building error messages
     * @param value Value to append to the error message
     * @return Reference to this ConfigError for chaining
     */
    template<typename T>
    ConfigError& operator<<(const T& value) {
        stream_ << value;
        has_errors_ = true;
        return *this;
    }

    /**
     * Check if any errors have been accumulated
     * @return True if errors have been recorded
     */
    bool HasErrors() const {
        return has_errors_;
    }

    /**
     * Get the error message string
     * @return The accumulated error message
     */
    std::string getMessage() const {
        return stream_.str();
    }

    /**
     * std::exception interface - return error message as C string
     * @return C-style string containing the error message
     */
    const char* what() const noexcept override {
        // Cache the string to ensure the pointer remains valid
        cached_message_ = stream_.str();
        return cached_message_.c_str();
    }

private:
    mutable std::stringstream stream_;
    mutable std::string cached_message_;
    bool has_errors_ = false;
};

/**
 * Stream manipulator support for ConfigError
 * Allows using std::endl with ConfigError instances
 */
inline ConfigError& operator<<(ConfigError& error, std::ostream& (*manip)(std::ostream&)) {
    if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::endl)) {
        error << '\n';
    } else if (manip == static_cast<std::ostream& (*)(std::ostream&)>(std::flush)) {
        // For flush, we don't need to do anything special since we're not buffering
    }
    return error;
}