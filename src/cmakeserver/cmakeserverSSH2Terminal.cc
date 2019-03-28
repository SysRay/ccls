#include "ICMakeserverTerminal.hh"
#include "config.hh"
#include "log.hh"

#include <vector>
#include <libssh/libssh.h>
#include <array>
#include <atomic>

class ssh2Process
  : public ICMakeServerTerminal
{
private:
  std::string m_path;
  ssh_session m_session;
  ssh_channel m_channel;
  int m_sock;
  std::array<char,2048> m_bufferRead;
  std::atomic<bool> m_isRunning;

public:
  std::string read_blocking() final;
  bool write_blocking(std::string const&) final;
  bool write_blocking(std::string &&) final;
  bool restart() final;
  void deinit() final {
    m_isRunning = false;
  }

  bool init(std::string const& sshdir, std::string const &path, std::string const& hostname, std::string const& username, std::string const& password, int const port);
 
  ~ssh2Process() {
    if (m_channel != nullptr) {
      ssh_channel_close(m_channel);
      ssh_channel_free(m_channel);
    }
    if (m_isValid) {
      ssh_disconnect(m_session);
      ssh_free(m_session);
    }
  }
};

std::string ssh2Process::read_blocking(){
  if (!m_isValid) return {};

  std::string retData;
  int nbytes;
  do
  {
    nbytes = ssh_channel_read_timeout(m_channel, m_bufferRead.data(), m_bufferRead.size(), 0, 100);
    if (nbytes == 0 && !m_isRunning) {
      break;
	}
    else if (nbytes == SSH_ERROR) {
      LOG_S(ERROR) << "[libSSH] couldn't read! " << ssh_get_error(m_session);
      break;
    }
    else if (nbytes == SSH_AGAIN) {
      continue;
    } 
	else if (nbytes == SSH_AGAIN) {
      continue;
    }

    retData += std::string(m_bufferRead.data(), nbytes);
  } while (nbytes == 0);

  return retData;
}

bool ssh2Process::write_blocking(std::string const& data) {
  if (!m_isValid) return false;

  auto ret = ssh_channel_write(m_channel, data.data(), data.size());
  if (ret == SSH_ERROR) {
    LOG_S(ERROR) << "[libSSH] couldn't write! " << ssh_get_error(m_session);
  }
  else if (ret != data.size()) {
    LOG_S(ERROR) << "[libSSH] couldn't write whole data!";
  }

  return true;
}
bool ssh2Process::write_blocking(std::string && data) {
  if (!m_isValid) return false;

  auto ret = ssh_channel_write(m_channel, data.data(), data.size());
  if (ret == SSH_ERROR) {
    LOG_S(ERROR) << "[libSSH] couldn't write! " << ssh_get_error(m_session);
  }
  else if (ret != data.size()) {
    LOG_S(ERROR) << "[libSSH] couldn't write whole data!";
  }

  return true;
}
bool verify_knownhost(ssh_session session)
{
  enum ssh_known_hosts_e state;
  unsigned char *hash = NULL;
  ssh_key srv_pubkey = NULL;
  size_t hlen;
  char buf[10];
  char *hexa;
  char *p;
  int cmp;
  int rc;

  if (rc = ssh_get_server_publickey(session, &srv_pubkey) < 0) {
    LOG_S(ERROR) << "[libssh] << ssh_get_server_publickey error: " << rc;
    return false;
  }

  rc = ssh_get_publickey_hash(srv_pubkey, SSH_PUBLICKEY_HASH_SHA1, &hash, &hlen);
  ssh_key_free(srv_pubkey);

  if (rc < 0) {
    LOG_S(ERROR) << "[libssh] << ssh_get_publickey_hash error: " << rc;
    return false;
  }


  state = ssh_session_is_known_server(session);
  switch (state) {
  case SSH_KNOWN_HOSTS_OK:
    /* OK */
    break;
  case SSH_KNOWN_HOSTS_CHANGED:
    LOG_S(ERROR) << "Host key for server changed: it is now:";
    ssh_print_hexa("Public key hash", hash, hlen);
    hexa = ssh_get_hexa(hash, hlen);

    rc = ssh_session_update_known_hosts(session);
    if (rc < 0) {
      LOG_S(ERROR) << "Error " << strerror(errno);
      ssh_clean_pubkey_hash(&hash);
      return false;
    }
    break;
  case SSH_KNOWN_HOSTS_OTHER:
    LOG_S(ERROR) << "The host key for this server was not found but an othertype of key exists.";
    LOG_S(ERROR) << "An attacker might change the default server key to confuse your client into thinking the key does not exist";
    ssh_clean_pubkey_hash(&hash);
    return false;
  case SSH_KNOWN_HOSTS_NOT_FOUND:
    LOG_S(ERROR) << "Could not find known host file.";
    LOG_S(ERROR) << "If you accept the host key here, the file will be automatically created.";
    /* FALL THROUGH to SSH_SERVER_NOT_KNOWN behavior */
  case SSH_KNOWN_HOSTS_UNKNOWN:
    hexa = ssh_get_hexa(hash, hlen);

    rc = ssh_session_update_known_hosts(session);
    if (rc < 0) {
      LOG_S(ERROR) << "Error " <<  strerror(errno);
      ssh_clean_pubkey_hash(&hash);
      return false;
    }
    break;
  case SSH_KNOWN_HOSTS_ERROR:
    LOG_S(ERROR) << "Error " << ssh_get_error(session);
    ssh_clean_pubkey_hash(&hash);
    return false;
  }
  ssh_clean_pubkey_hash(&hash);
  return true;
}

bool ssh2Process::init(std::string const& sshdir, std::string const &path, std::string const& hostname, std::string const& username, std::string const& password, int const port) {
  if (m_path.empty()) {
    m_path = path + CMAKE_PARAM_SERVERCALL;

    m_session = ssh_new();
    if (m_session == nullptr) return false;

    ssh_options_set(m_session, SSH_OPTIONS_SSH_DIR, sshdir.data());
    ssh_options_set(m_session, SSH_OPTIONS_USER, username.data());
    ssh_options_set(m_session, SSH_OPTIONS_HOST, hostname.data());
    ssh_options_set(m_session, SSH_OPTIONS_PORT, &port);

    // Connect
    if (ssh_connect(m_session) != SSH_OK)
    {
      LOG_S(ERROR)  << "Error connecting to remote: " << ssh_get_error(m_session);
      return false;
    }

    // Verify the server's identity
    // For the source code of verify_knownhost(), check previous example
    if (verify_knownhost(m_session) < 0)
    {
      ssh_disconnect(m_session);
      ssh_free(m_session);
      return false;
    }

    // Authenticate ourselves
    if (ssh_userauth_publickey_auto(m_session, username.c_str(), password.c_str()) !=SSH_AUTH_SUCCESS) {
      LOG_S(ERROR) << "Error authenticating! " << ssh_get_error(m_session);
      ssh_disconnect(m_session);
      ssh_free(m_session);
      return false;
    }
    /*
    if (ssh_userauth_password(m_session, username.c_str(),password.c_str()) != SSH_AUTH_SUCCESS)
    {
      LOG_S(ERROR) << "Error authenticating! " << ssh_get_error(m_session);
      ssh_disconnect(m_session);
      ssh_free(m_session);
      return false;
    }
    */
    int rc;
    m_channel = ssh_channel_new(m_session);
    if (m_channel == NULL)
      return SSH_ERROR;
    rc = ssh_channel_open_session(m_channel);
    if (rc != SSH_OK)
    {
      ssh_channel_free(m_channel);
      m_channel = nullptr;
      return rc;
    }

    rc = ssh_channel_request_exec(m_channel, m_path.c_str());
    if (rc != SSH_OK)
    {
      ssh_channel_close(m_channel);
      ssh_channel_free(m_channel);
      m_channel = nullptr;
      return rc;
    }
    m_isRunning = true;
    m_isValid = true;
    return true;
  }
  return false;
}

bool ssh2Process::restart() {
  if (!m_isValid) return false;

  int rc;
  ssh_channel_close(m_channel);

  rc = ssh_channel_open_session(m_channel);
  if (rc != SSH_OK)
  {
    ssh_channel_free(m_channel);
    m_channel = nullptr;
    return rc;
  }
  rc = ssh_channel_request_exec(m_channel, m_path.c_str());
  if (rc != SSH_OK)
  {
    ssh_channel_close(m_channel);
    ssh_channel_free(m_channel);
    m_channel = nullptr;
    return rc;
  }

  return true;
}

// Factory
std::unique_ptr<ICMakeServerTerminal> createRemoteCMakeServerTerminal(
    std::string const &sshdir, std::string const &path,
    std::string const &hostname, std::string const &username,
    std::string const &password, int const port) {
  auto inst = std::make_unique<ssh2Process>();

  if (inst->init(sshdir, path, hostname, username, password, port) == true) {
    return inst;
  }

  return {};
}

