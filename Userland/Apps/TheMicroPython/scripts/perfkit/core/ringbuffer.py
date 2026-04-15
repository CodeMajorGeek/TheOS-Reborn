class RingBuffer:
    def __init__(self, capacity):
        self.capacity = capacity if capacity > 0 else 1
        self._data = []

    def push(self, item):
        self._data.append(item)
        while len(self._data) > self.capacity:
            self._data.pop(0)

    def items(self):
        return self._data
