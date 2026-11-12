// Wire/Windows integer representation, not a display or codec capability.
export const maximumGeometrySide = 0x7ffffffe;
export const maximumWireInteger = Number.MAX_SAFE_INTEGER;
export const minimumGeometryCoordinate = -0x80000000;
export const maximumGeometryCoordinate = 0x7fffffff;
export function videoPlacement(width, height, x, y) {
  return [width,height,x,y].every(Number.isSafeInteger) && width > 0 && height > 0 &&
    x >= minimumGeometryCoordinate && y >= minimumGeometryCoordinate &&
    x <= maximumGeometryCoordinate - width && y <= maximumGeometryCoordinate - height;
}
export const boundedInteger = (value, minimum, maximum = maximumWireInteger) =>
  Number.isSafeInteger(value) && value >= minimum && value <= maximum;
export function videoGeometry(width, height, minimumWidth = 2, minimumHeight = 2) {
  return boundedInteger(width, minimumWidth, maximumGeometrySide) &&
    boundedInteger(height, minimumHeight, maximumGeometrySide) &&
    !(width % 2) && !(height % 2) && width <= Math.floor(maximumWireInteger / height);
}
export function aggregatePixels(outputs) {
  let total = 0;
  for (const output of outputs) {
    if (!videoGeometry(output.width, output.height, 320, 180)) return null;
    const pixels = output.width * output.height;
    if (pixels > maximumWireInteger - total) return null;
    total += pixels;
  }
  return total;
}
