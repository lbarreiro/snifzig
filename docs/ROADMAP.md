# Plano de trabalho

## 0. Captura passiva e indicadores horários

O componente externo em `components/snifzig_capture` ouve o canal Zigbee configurado em modo passivo. Não adere à rede nem transmite.

O firmware mantém apenas estatísticas em RAM durante uma hora efetiva de captura:
- total de tramas e classificação básica do tipo MAC;
- mínimo, média, máximo e distribuição de RSSI/LQI;
- erros reportados pelo driver.

No fim do período liga o Wi-Fi, publica um resumo em texto normal no ntfy, desliga o Wi-Fi e retoma a captura. Se o envio falhar, conserva os contadores e tenta novamente após cinco minutos. OTA continua disponível ao manter BOOT premido por três segundos, abrindo uma janela de dez minutos.

## 1. Interpretar a observação local

Comparar resumos de várias horas no mesmo local e observar tendências de RSSI/LQI e volume/tipos de tráfego. O Snifzig só ouve as tramas que chegam ao seu ponto de instalação; não confirma entregas nem conhece diretamente rotas ou retransmissões entre outros nós.

A publicação e a janela OTA interrompem a escuta do Snifzig durante alguns segundos, mas não alteram o funcionamento da rede Zigbee.

## Decisões confirmadas

- Placa: Seeed Studio XIAO ESP32-C6.
- Canal inicial: 11.
- Antena: cerâmica integrada.
- Arquitetura: ESPHome com componente externo.
- Wi-Fi desligado durante a captura.
- Sem guardar tramas completas nem anexar CSV; apenas contadores temporários em RAM.
- Notificações ntfy em texto, uma por hora de captura.
- Endpoint e credenciais ficam apenas na configuração local e em secrets, nunca no Git.
