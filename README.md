# AdLcom 패치판
A AdLib Driver for serial port.

이 adlcom 패치판은 adlipt를 시리얼포트용으로 패치한 adlcom을 쉐도우 레지스터 캐시 최적화한 패치판입니다.
시리얼널모뎀케이블을 통해서 PC에서 재생할 수 있고, 혹은 MCU를 이용한 에뮬을 통해서 재생가능합니다.
저는 esp32-s3에 ymfm코어를 올려서 재생할 것입니다. 아마도 opl3duo 도 가능할 것으로 보입니다만, 제게는 없어서 모르겠습니다.
실행은 adlcom opl3 로 하시면 되고, 경우에 따라서 adlcom opl3 nopatch로도 사용해 보시기 바랍니다.






<img width="1199" height="1519" alt="esp_ymfm" src="https://github.com/user-attachments/assets/7f283c5a-6328-4a1d-a0d5-45ce7220ee77" />

저항과 콘덴서는 안정성을 올리기 위한 것으로 없으면 생략하셔도 됩니다.







## Copying

Copyright © 2023 Jose Phillips
Copyright © 2020 Peter De Wachter (pdewacht@gmail.com)

Permission to use, copy, modify, and distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
