I (10431) realtime_voice: activation phrase matched: Hi ESP
I (10431) realtime_voice: record_start max_ms=5000
I (11471) voice_frontend: diag: mic_read_ok=55 read_fail=0 feed_ok=103 fetch_ok=102 feed_pending_frames=64 q_drop=2 loop_hz=11 wake=16/16 fetch_timeout=0 fetch_yield=106 age(read/feed/fetch)=58ms/0ms/6ms
I (11561) voice_frontend: voice_fe yield: progress=54 no_progress=0
I (11841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
I (13841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
I (15081) voice_session: record_end frames=48128 reason=silence
I (15081) voice_session: frontend_pause_begin: reason=pause_oneshot_upload running=1 paused=0 stopping=0 state=2 cap_task=0x3fca7c48 fetch_task=0x3fca7af0 queue=1
I (15081) voice_frontend: frontend_pause_request: reason=pause_oneshot_upload running=1 paused=0 state=2 cap_task=0x3fca7c48 fetch_task=0x3fca7af0 int_free=39631 int_largest=17408 spiram_free=6369908
I (15141) voice_frontend: frontend_pause_result: reason=pause_oneshot_upload waited_ms=60 cap_ack=1 fetch_ack=1
I (15141) voice_session: frontend_pause_result: reason=pause_oneshot_upload running=1 paused=1 stopping=0 state=4 cap_task=0x3fca7c48 fetch_task=0x3fca7af0 queue=1 result=done
D (15161) voice_request: wav_from_pcm ok: frames=48128 data=96256 file=96300 rate=16000
I (15381) voice_request: upload_start frames=48128 wav=96300 b64=128422 body=131753 rate=16000 endpoint=https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions model=qwen3.5-omni-flash
D (15401) HTTP_CLIENT: set post file length = 131753
D (15401) HTTP_CLIENT: Begin connect to: https://dashscope.aliyuncs.com:443
D (15661) esp-tls-mbedtls: Use certificate bundle
I (15841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (16231) intr_alloc: Connected src 76 to int 18 (cpu 0)
D (16331) esp-x509-crt-bundle: 43 certificates in bundle
I (16351) esp-x509-crt-bundle: Certificate validated
D (17371) HTTP_CLIENT: Write header[5]: POST /compatible-mode/v1/chat/completions HTTP/1.1
User-Agent: ESP32 HTTP Client/1.0
Host: dashscope.aliyuncs.com
Authorization: Bearer sk-3991228d25b54e72b77a0df2ef93da33
Content-Type: application/json
Content-Length: 131753


D (17381) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 0, size 4096
D (17391) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 4096, size 4096
D (17551) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 8192, size 4096
D (17581) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 12288, size 4096
D (17711) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 16384, size 4096
I (17841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (17971) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 20480, size 4096
D (18291) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 24576, size 4096
D (18501) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 28672, size 4096
D (18511) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 32768, size 4096
D (18741) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 36864, size 4096
D (18771) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 40960, size 4096
D (18871) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 45056, size 4096
D (18881) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 49152, size 4096
D (19001) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 53248, size 4096
I (19841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (20461) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 57344, size 4096
D (20471) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 61440, size 4096
D (20591) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 65536, size 4096
D (20981) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 69632, size 4096
D (21531) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 73728, size 4096
D (21551) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 77824, size 4096
I (21841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (22151) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 81920, size 4096
D (22361) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 86016, size 4096
D (22601) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 90112, size 4096
D (23021) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 94208, size 4096
D (23031) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 98304, size 4096
D (23481) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 102400, size 4096
I (23841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (24181) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 106496, size 4096
D (24641) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 110592, size 4096
D (24651) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 114688, size 4096
D (24951) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 118784, size 4096
D (25301) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 122880, size 4096
D (25311) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 126976, size 4096
I (25841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (27461) esp-tls-mbedtls: Fragmenting data of excessive size :131753, offset: 131072, size 681
I (27841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (28451) HTTP_CLIENT: on_message_begin
D (28451) HTTP_CLIENT: HEADER=vary:Origin
D (28451) HTTP_CLIENT: HEADER=x-request-id:e6c59218-a945-9c5b-8122-a46d647a9e4c
D (28451) HTTP_CLIENT: HEADER=content-type:text/event-stream;charset=utf-8
D (28451) HTTP_CLIENT: HEADER=x-dashscope-call-gateway:true
D (28451) HTTP_CLIENT: HEADER=req-cost-time:11021
D (28451) HTTP_CLIENT: HEADER=req-arrive-time:1778086665588
D (28451) HTTP_CLIENT: HEADER=resp-start-time:1778086676610
D (28451) HTTP_CLIENT: HEADER=x-envoy-upstream-service-time:274
D (28461) HTTP_CLIENT: HEADER=date:Wed, 06 May 2026 16:57:56 GMT
D (28461) HTTP_CLIENT: HEADER=server:istio-envoy
D (28461) HTTP_CLIENT: HEADER=transfer-encoding:chunked
D (28461) HTTP_CLIENT: http_on_headers_complete, status=200, offset=372, nread=372
D (28461) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (28461) HTTP_CLIENT: http_on_body 293
D (28461) HTTP_CLIENT: http_on_chunk_complete
D (28461) HTTP_CLIENT: content_length = -1
D (28461) HTTP_CLIENT: data_process=293, content_length=-1
D (28671) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (28671) HTTP_CLIENT: http_on_body 310
D (28671) HTTP_CLIENT: http_on_chunk_complete
D (28671) HTTP_CLIENT: data_process=603, content_length=-1
D (29771) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (29771) HTTP_CLIENT: http_on_body 4090
D (29771) HTTP_CLIENT: data_process=4693, content_length=-1
D (29771) transport_base: remain data in cache, need to read again
D (29771) HTTP_CLIENT: http_on_body 4096
D (29771) HTTP_CLIENT: data_process=8789, content_length=-1
D (29771) transport_base: remain data in cache, need to read again
D (29771) HTTP_CLIENT: http_on_body 4096
I (29781) voice_request: sse line buffer grown: cap=16384 grows=1
D (29781) HTTP_CLIENT: data_process=12885, content_length=-1
D (29781) transport_base: remain data in cache, need to read again
D (29781) HTTP_CLIENT: http_on_body 2312
D (29781) HTTP_CLIENT: http_on_chunk_complete
D (29781) HTTP_CLIENT: data_process=15197, content_length=-1
I (29841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (30401) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (30401) HTTP_CLIENT: http_on_body 4090
I (30411) voice_request: sse line buffer grown: cap=32768 grows=2
D (30411) HTTP_CLIENT: data_process=19287, content_length=-1
D (30411) transport_base: remain data in cache, need to read again
D (30411) HTTP_CLIENT: http_on_body 2145
I (30491) voice_session: playback buffer start: internal_free=14807 internal_largest=7936 spiram_free=5895196 chunk_frames=7680 audio_frames_total=0
D (30491) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=1
D (30491) HTTP_CLIENT: http_on_chunk_complete
D (30491) HTTP_CLIENT: data_process=21432, content_length=-1
D (31391) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (31391) HTTP_CLIENT: http_on_body 4090
D (31391) HTTP_CLIENT: data_process=25522, content_length=-1
D (31391) transport_base: remain data in cache, need to read again
D (31391) HTTP_CLIENT: http_on_body 4096
D (31391) HTTP_CLIENT: data_process=29618, content_length=-1
D (31391) transport_base: remain data in cache, need to read again
D (31391) HTTP_CLIENT: http_on_body 4096
D (31391) HTTP_CLIENT: data_process=33714, content_length=-1
D (31391) transport_base: remain data in cache, need to read again
D (31391) HTTP_CLIENT: http_on_body 4096
D (31391) HTTP_CLIENT: data_process=37810, content_length=-1
D (31681) HTTP_CLIENT: http_on_body 4096
D (31681) HTTP_CLIENT: data_process=41906, content_length=-1
D (31681) transport_base: remain data in cache, need to read again
D (31681) HTTP_CLIENT: http_on_body 355
D (31741) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=3
D (31741) HTTP_CLIENT: http_on_chunk_complete
D (31741) HTTP_CLIENT: data_process=42261, content_length=-1
I (31841) app_core: stack watermark[net_mon]: free_words=1928 free_bytes=1928
I (31841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (32901) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (32901) HTTP_CLIENT: http_on_body 4090
D (32901) HTTP_CLIENT: data_process=46351, content_length=-1
D (32901) transport_base: remain data in cache, need to read again
D (32901) HTTP_CLIENT: http_on_body 4096
D (32901) HTTP_CLIENT: data_process=50447, content_length=-1
D (32901) transport_base: remain data in cache, need to read again
D (32901) HTTP_CLIENT: http_on_body 4096
D (32901) HTTP_CLIENT: data_process=54543, content_length=-1
D (32901) transport_base: remain data in cache, need to read again
D (32901) HTTP_CLIENT: http_on_body 4096
D (32901) HTTP_CLIENT: data_process=58639, content_length=-1
D (33121) HTTP_CLIENT: http_on_body 4096
D (33131) HTTP_CLIENT: data_process=62735, content_length=-1
D (33131) transport_base: remain data in cache, need to read again
D (33131) HTTP_CLIENT: http_on_body 355
D (33201) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=4
I (33201) voice_session: playback arm: startup_wait_ms=2710 buffered_ms=960 buffered_frames=23040
I (33201) app_core: resource event: voice started
I (33201) audio_output: speaker ownership acquired by realtime_voice (worker=stopped, speaker_open=1)
I (33771) voice_speaker: voice playback volume diag: target=-15.0 current=-15.0 hw=15 muted=0
I (33771) voice_speaker: spk_open workbufs: pop=240 hw=761 stereo=1522 resampler=1
I (33771) voice_speaker: spk: codec open success, rate=44100 resampler=1
D (33771) HTTP_CLIENT: http_on_chunk_complete
D (33771) HTTP_CLIENT: data_process=63090, content_length=-1
W (33781) voice_playout_drain: playout ring low: avail=950ms threshold=1200ms
I (33841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
D (34101) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (34101) HTTP_CLIENT: http_on_body 4090
D (34101) HTTP_CLIENT: data_process=67180, content_length=-1
D (34101) transport_base: remain data in cache, need to read again
D (34101) HTTP_CLIENT: http_on_body 4096
D (34101) HTTP_CLIENT: data_process=71276, content_length=-1
D (34101) transport_base: remain data in cache, need to read again
D (34101) HTTP_CLIENT: http_on_body 4096
D (34101) HTTP_CLIENT: data_process=75372, content_length=-1
D (34111) transport_base: remain data in cache, need to read again
D (34111) HTTP_CLIENT: http_on_body 4096
D (34111) HTTP_CLIENT: data_process=79468, content_length=-1
D (34191) HTTP_CLIENT: http_on_body 4096
D (34191) HTTP_CLIENT: data_process=83564, content_length=-1
D (34191) transport_base: remain data in cache, need to read again
D (34191) HTTP_CLIENT: http_on_body 355
D (34271) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=3
D (34271) HTTP_CLIENT: http_on_chunk_complete
D (34271) HTTP_CLIENT: data_process=83919, content_length=-1
I (34411) voice_playout_drain: spk: pcm peak input=12863 output=12850 frames=441
D (34451) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (34451) HTTP_CLIENT: http_on_body 4090
D (34451) HTTP_CLIENT: data_process=88009, content_length=-1
D (34451) transport_base: remain data in cache, need to read again
D (34451) HTTP_CLIENT: http_on_body 4096
D (34451) HTTP_CLIENT: data_process=92105, content_length=-1
D (34451) transport_base: remain data in cache, need to read again
D (34461) HTTP_CLIENT: http_on_body 4096
D (34461) HTTP_CLIENT: data_process=96201, content_length=-1
D (34461) transport_base: remain data in cache, need to read again
D (34461) HTTP_CLIENT: http_on_body 4096
D (34461) HTTP_CLIENT: data_process=100297, content_length=-1
D (34521) HTTP_CLIENT: http_on_body 4096
D (34531) HTTP_CLIENT: data_process=104393, content_length=-1
D (34531) transport_base: remain data in cache, need to read again
D (34531) HTTP_CLIENT: http_on_body 355
D (34601) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=3
D (34601) HTTP_CLIENT: http_on_chunk_complete
D (34611) HTTP_CLIENT: data_process=104748, content_length=-1
D (34931) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (34931) HTTP_CLIENT: http_on_body 4090
D (34931) HTTP_CLIENT: data_process=108838, content_length=-1
D (34931) transport_base: remain data in cache, need to read again
D (34931) HTTP_CLIENT: http_on_body 4096
D (34931) HTTP_CLIENT: data_process=112934, content_length=-1
D (34931) transport_base: remain data in cache, need to read again
D (34931) HTTP_CLIENT: http_on_body 4096
D (34931) HTTP_CLIENT: data_process=117030, content_length=-1
D (34931) transport_base: remain data in cache, need to read again
D (34941) HTTP_CLIENT: http_on_body 4096
D (34941) HTTP_CLIENT: data_process=121126, content_length=-1
I (35031) voice_playout_drain: spk: pcm peak input=122 output=123 frames=441
D (35251) HTTP_CLIENT: http_on_body 4096
D (35251) HTTP_CLIENT: data_process=125222, content_length=-1
D (35251) transport_base: remain data in cache, need to read again
D (35251) HTTP_CLIENT: http_on_body 355
D (35341) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=1
D (35341) HTTP_CLIENT: http_on_chunk_complete
D (35341) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (35341) HTTP_CLIENT: http_on_body 3733
D (35341) HTTP_CLIENT: data_process=129310, content_length=-1
D (35341) transport_base: remain data in cache, need to read again
D (35341) HTTP_CLIENT: http_on_body 641
D (35341) HTTP_CLIENT: http_on_chunk_complete
D (35341) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (35341) HTTP_CLIENT: http_on_body 3447
D (35341) HTTP_CLIENT: data_process=133398, content_length=-1
D (35341) transport_base: remain data in cache, need to read again
D (35341) HTTP_CLIENT: http_on_body 4096
D (35351) HTTP_CLIENT: data_process=137494, content_length=-1
D (35471) HTTP_CLIENT: http_on_body 4096
D (35471) HTTP_CLIENT: data_process=141590, content_length=-1
D (35471) transport_base: remain data in cache, need to read again
D (35471) HTTP_CLIENT: http_on_body 4096
D (35471) HTTP_CLIENT: data_process=145686, content_length=-1
D (35471) transport_base: remain data in cache, need to read again
D (35471) HTTP_CLIENT: http_on_body 720
D (35551) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=2
D (35551) HTTP_CLIENT: http_on_chunk_complete
D (35551) HTTP_CLIENT: data_process=146406, content_length=-1
I (35671) voice_playout_drain: spk: pcm peak input=4287 output=4181 frames=441
D (35751) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (35751) HTTP_CLIENT: http_on_body 4090
D (35751) HTTP_CLIENT: data_process=150496, content_length=-1
D (35751) transport_base: remain data in cache, need to read again
D (35751) HTTP_CLIENT: http_on_body 4096
D (35751) HTTP_CLIENT: data_process=154592, content_length=-1
D (35751) transport_base: remain data in cache, need to read again
D (35751) HTTP_CLIENT: http_on_body 4096
D (35761) HTTP_CLIENT: data_process=158688, content_length=-1
D (35761) transport_base: remain data in cache, need to read again
D (35761) HTTP_CLIENT: http_on_body 4096
D (35761) HTTP_CLIENT: data_process=162784, content_length=-1
W (35791) voice_playout_drain: playout ring low: avail=980ms threshold=1200ms
I (35841) airplay_service: playback state[desired_off]: receiver=discoverable desired=0 running=0 output_active=0
W (35851) env_monitor: SHT30 probe failed: addr=0x44 no ACK on shared bus
W (35851) env_monitor: QMP6988 probe failed: addrs=0x70/0x56 no ACK on shared bus
W (35851) env_monitor: sensor init retry pending: err=ESP_ERR_NOT_FOUND next_retry_ms=30000
D (36001) HTTP_CLIENT: http_on_body 4096
D (36001) HTTP_CLIENT: data_process=166880, content_length=-1
D (36001) transport_base: remain data in cache, need to read again
D (36001) HTTP_CLIENT: http_on_body 4096
D (36081) voice_session: oneshot audio_cb enqueue done: frames=7680 wait_ms=3
D (36081) HTTP_CLIENT: data_process=170976, content_length=-1
D (36081) transport_base: remain data in cache, need to read again
D (36091) HTTP_CLIENT: http_on_body 4096
D (36091) HTTP_CLIENT: data_process=175072, content_length=-1
D (36091) transport_base: remain data in cache, need to read again
D (36091) HTTP_CLIENT: http_on_body 3420
D (36121) voice_session: oneshot audio_cb enqueue done: frames=3840 wait_ms=1
D (36131) HTTP_CLIENT: http_on_chunk_complete
D (36131) HTTP_CLIENT: http_on_chunk_header, chunk_length
D (36131) HTTP_CLIENT: http_on_chunk_complete
D (36131) HTTP_CLIENT: http_on_message_complete, parser=0x3fce3f50
I (36131) voice_request: oneshot stream summary: text_chunks=3 audio_chunks=9 audio_decode_ok=9 audio_decode_fail=0 audio_frames_decoded=65280 audio_frames_enqueued=65280 audio_frames_total=65280 sse_max=20827 sse_grow=2 sse_drop=0 delta_no_audio=0
I (36131) voice_request: upload_done latency=20996ms text="好的，请问您有什么需要帮助的吗？"
I (36131) voice_request: upload_cleanup status=ESP_OK http_completed=1 wav=0x3c3be348 b64=0x3c3d634c body=0x3c415354 line_buf=0x3c401c68 wav_len=96300 b64_len=128422 body_len=131753
I (36141) voice_session: oneshot response audio closed: playout_avail=30000
I (36141) voice_session: AI reply: "好的，请问您有什么需要帮助的吗？"
I (36331) voice_playout_drain: spk: pcm peak input=14399 output=14370 frames=441
I (36821) voice_playout_drain: spk: pcm peak input=16127 output=16139 frames=441
I (37321) voice_playout_drain: spk: pcm peak input=7359 output=6919 frames=441
I (37731) I2S_IF: Pending out channel for in channel running
I (37741) audio_output: I2S bus: FULL_DUPLEX → RX_ONLY (rx=1 tx=0)
I (37741) audio_output: CoreS3 speaker closed owner=external
I (37741) audio_output: speaker ownership released (resume_worker=0, speaker_open=0)
I (37741) app_core: resource event: voice stopped

***ERROR*** A stack overflow in task voice_playback has been detected.


Backtrace: 0x4037e9ed:0x3c43d370 0x4037e9b5:0x3c43d390 0x4037f986:0x3c43d3b0 0x40380ea7:0x3c43d430 0x4037fa8c:0x3c43d450 0x4037fa82:0x0000000c |<-CORRUPTED
