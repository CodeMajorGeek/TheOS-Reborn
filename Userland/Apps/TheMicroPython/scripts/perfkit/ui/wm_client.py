import theos

# Errnos TheOS (libc) — éviter import errno sur ports minimalistes.
_ECONNRESET = 104
_ENOTCONN = 107
_ETIMEDOUT = 110
_EPIPE = 32


class WMClient:
    def __init__(self, role=None):
        self._role = theos.WS_ROLE_GENERIC if role is None else int(role)
        self._connected = False
        self._window_id = None
        self._win_args = None

    def window_id(self):
        return self._window_id

    @staticmethod
    def _os_errno(exc):
        try:
            return int(exc.args[0])
        except Exception:
            return -1

    def _recoverable_ipc(self, code):
        return code in (_ENOTCONN, _ECONNRESET, _EPIPE, _ETIMEDOUT)

    def connect(self, retries=200, retry_sleep_ms=20):
        # WindowServer socket can appear shortly after shell startup.
        for _ in range(int(retries)):
            try:
                theos.window_connect(self._role)
                self._connected = True
                return
            except OSError:
                theos.time_sleep_ms(int(retry_sleep_ms))
        theos.window_connect(self._role)
        self._connected = True

    def create_window(self, title, width, height, x, y):
        if not self._connected:
            self.connect()
        self._win_args = (title, int(width), int(height), int(x), int(y))
        self._window_id = int(theos.window_create(title, int(width), int(height), int(x), int(y)))
        return self._window_id

    def _reopen_window(self):
        if not self._win_args:
            raise OSError("window params missing")
        t, w, h, x, y = self._win_args
        self._window_id = int(theos.window_create(t, w, h, x, y))
        return self._window_id

    def _recover_wm_session(self):
        self._connected = False
        self._window_id = None
        try:
            theos.window_disconnect()
        except Exception:
            pass
        self.connect()
        self._reopen_window()

    def set_text(self, text):
        if self._window_id is None:
            raise OSError("window not created")
        try:
            theos.window_set_text(int(self._window_id), text)
        except OSError as e:
            code = self._os_errno(e)
            if not self._recoverable_ipc(code):
                raise
            self._recover_wm_session()
            theos.window_set_text(int(self._window_id), text)

    def sleep_ms(self, ms):
        theos.time_sleep_ms(int(ms))

    def close(self):
        if self._connected:
            try:
                theos.window_disconnect()
            finally:
                self._connected = False
                self._window_id = None
                self._win_args = None
