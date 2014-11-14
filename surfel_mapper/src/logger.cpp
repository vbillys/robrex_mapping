#include "logger_impl.hpp"

//Instantiate necessary functions
template void Logger::log<int>(const std::string &field, const int value) ; 
template void Logger::log<unsigned long>(const std::string &field, const unsigned long value) ; 
template void Logger::log<unsigned int>(const std::string &field, const unsigned int value) ; 
template void Logger::log<float>(const std::string &field, const float value) ; 
template void Logger::log<double>(const std::string &field, const double value) ; 
template void Logger::log<std::string>(const std::string &field, const std::string value) ; 
