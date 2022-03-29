#include "Arduino.h"

const char HTTP_UPDATE[] PROGMEM = R"rawliteral(
    <html lang="en">

  <head>
    <meta name="format-detection" content="telephone=no">
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
    <title>%HTML_HEAD_TITLE%</title>
    <script>
      function c(l) {
        document.getElementById('s').value = l.getAttribute('data-ssid') || l.innerText || l.textContent;
        p = l.nextElementSibling.classList.contains('l');
        document.getElementById('p').disabled = !p;
        if (p) document.getElementById('p').focus();
      }

    </script>
    <style>
    %CSS_TEMPLATE%
    </style>
  </head>

  <body class="invert">
    <div class="wrap">
      <h1>%HTML_HEAD_TITLE%</h1>
      Upload New Firmware<br>
      <form method="POST" action="u" enctype="multipart/form-data" onchange="(function(el){document.getElementById('uploadbin').style.display = el.value=='' ? 'none' : 'initial';})(this)">
      <input type="file" name="update" accept=".bin,application/octet-stream"><button id="uploadbin" type="submit" class="h D">Update</button></form>
    </div>
  </body>

</html>
)rawliteral";
