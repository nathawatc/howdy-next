#ifndef HOWDY_PAM_AUTH_FLOW_TESTING_HPP
#define HOWDY_PAM_AUTH_FLOW_TESTING_HPP

#ifdef HOWDY_PAM_TESTING

#    include <string>

namespace howdy::pam::testing {

    auto read_fd_to_string(int fd) -> std::string;
    auto helper_output_value(const std::string &output, const std::string &key) -> std::string;

}  // namespace howdy::pam::testing

#endif

#endif  // HOWDY_PAM_AUTH_FLOW_TESTING_HPP
