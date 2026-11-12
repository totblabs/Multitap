export class HttpError extends Error {
  constructor(status, code, message = code, details) { super(message); this.status = status; this.code = code; this.details = details; }
}
export function fields(input, allowed) {
  if (Object.keys(input).some(key => !allowed.includes(key))) throw new HttpError(400, 'unexpected_field');
}
