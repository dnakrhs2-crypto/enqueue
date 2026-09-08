import { performance } from "node:perf_hooks";
import type { FeedbackPayload } from "@elgato/streamdeck";

export interface FeedbackOutput { setFeedback(value: FeedbackPayload): Promise<void>; showAlert(): Promise<void> }
/** Per-encoder rolling budget includes alerts; only the newest feedback survives throttling. */
export class FeedbackRenderer {
  private desired: FeedbackPayload | undefined;
  private sent = "";
  private alertPending = false;
  private sending = false;
  private disposed = false;
  private timer: ReturnType<typeof setTimeout> | undefined;
  constructor(private readonly output: FeedbackOutput, private readonly calls: number[] = []) {}
  render(value: FeedbackPayload): void { this.desired = value; void this.flush(); }
  alert(): void { this.alertPending = true; void this.flush(); }
  dispose(): void { this.disposed = true; clearTimeout(this.timer); }
  private async flush(): Promise<void> {
    if (this.disposed || this.sending || this.timer) return;
    this.sending = true;
    try {
      while (!this.disposed && this.desired) {
        const value = this.desired, serialized = JSON.stringify(value), changed = serialized !== this.sent, alert = this.alertPending;
        if (!changed && !alert) break;
        const now = performance.now();
        while (this.calls.length && now - this.calls[0]! >= 1000) this.calls.shift();
        if (this.calls.length + Number(changed) + Number(alert) > 10) {
          this.timer = setTimeout(() => { this.timer = undefined; void this.flush(); }, Math.max(1, 1001 - (now - this.calls[0]!))); break;
        }
        if (changed) { this.calls.push(performance.now()); await this.output.setFeedback(value); this.sent = serialized; }
        if (this.disposed) break;
        if (alert) { this.alertPending = false; this.calls.push(performance.now()); await this.output.showAlert(); }
      }
    } catch { this.sent = ""; }
    finally { this.sending = false; }
  }
}
