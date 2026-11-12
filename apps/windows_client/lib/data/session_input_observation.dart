/// Bounded observation of input actually submitted by this RemoteSession.
/// It never sends input, records characters, or changes the input gate policy.
class SessionInputObservation {
  final _keys = <int>{};
  final _buttons = <String>{};
  bool get held => _keys.isNotEmpty || _buttons.isNotEmpty;
  void observe(Map<String, dynamic> event) {
    if (event['type'] == 'input.reset') {
      _keys.clear();
      _buttons.clear();
      return;
    }
    if (event['type'] != 'input' || event['down'] is! bool) return;
    final down = event['down'] == true;
    final vk = event['vk'];
    final button = event['button'];
    if (event['event'] == 'key' && vk is int && vk > 0 && vk <= 255) {
      down ? _keys.add(vk) : _keys.remove(vk);
    } else if (event['event'] == 'button' &&
        button is String &&
        const {'left', 'right', 'middle', 'x1', 'x2'}.contains(button)) {
      down ? _buttons.add(button) : _buttons.remove(button);
    }
  }
}
