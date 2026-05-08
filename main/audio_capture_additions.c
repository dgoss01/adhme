// ─────────────────────────────────────────
// ADD THESE TWO FUNCTIONS to the bottom of audio_capture.c
// (after audio_capture_poll_done)
// ─────────────────────────────────────────

bool audio_capture_is_idle(void)
{
    return (s_state == ACAP_IDLE);
}

i2s_chan_handle_t audio_capture_get_tx_handle(void)
{
    return s_tx_handle;
}
