// Windows/JSON integer representation only. Device admission is separate.
const maximumGeometrySide = 0x7ffffffe;
const maximumWireInteger = 9007199254740991;
const minimumGeometryCoordinate = -0x80000000;
const maximumGeometryCoordinate = 0x7fffffff;

bool videoPlacement(int width, int height, int x, int y) =>
    width > 0 &&
    height > 0 &&
    x >= minimumGeometryCoordinate &&
    y >= minimumGeometryCoordinate &&
    x <= maximumGeometryCoordinate - width &&
    y <= maximumGeometryCoordinate - height;

bool videoGeometry(
  int width,
  int height, {
  int minimumWidth = 2,
  int minimumHeight = 2,
}) =>
    width >= minimumWidth &&
    height >= minimumHeight &&
    width <= maximumGeometrySide &&
    height <= maximumGeometrySide &&
    width.isEven &&
    height.isEven &&
    width <= maximumWireInteger ~/ height;
