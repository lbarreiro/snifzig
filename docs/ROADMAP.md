# Plano de trabalho

## Recolha passiva e relatório por hora

O XIAO ESP32-C6 não se associa à rede Zigbee e não transmite. No arranque, tenta o Wi-Fi configurado; sem ligação, o ESPHome abre após 30 segundos o portal cativo aberto “Snifzig”. Ao estabelecer ligação Wi-Fi, o firmware desliga Wi-Fi e inicia a captura Zigbee.

O firmware mantém estatísticas em RAM por endereço de origem reconhecido (até 64):
- número de tramas;
- RSSI/LQI médio por endereço;
- classificação local indicativa.

Tramas não identificáveis, incluindo ACKs sem endereço de origem e formatos não reconhecidos, formam um único grupo. A média dos grupos dá o mesmo peso a cada dispositivo identificado e ao grupo não identificado. A cada hora efetiva de escuta, publica texto no ntfy e retoma a captura. Um toque no BOOT abre uma janela OTA de dez minutos.

## Interpretação

As classificações indicam a receção observada no local do Snifzig; não medem diretamente entregas ou rotas entre todos os nós. LQI é agregado por média, mas as faixas qualitativas permanecem indicativas e devem ser calibradas com as medições reais.

## Configuração

- Placa: Seeed Studio XIAO ESP32-C6.
- Canal: 11 por omissão no YAML local.
- Wi-Fi: credenciais do YAML; portal cativo de provisionamento aberto com SSID “Snifzig”.
- Ntfy: texto sem anexos; endpoint definido localmente.
