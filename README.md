# snifzig

Projeto experimental para observar tráfego Zigbee 802.15.4 com um Seeed Studio XIAO ESP32-C6, preservar tramas capturadas em RAM e enviar blocos CSV como anexo para ntfy.

## Objetivo

- Ouvir passivamente a rede Zigbee no canal 11, sem aderir à rede nem transmitir.
- Guardar cada trama recebida tal como o rádio a disponibiliza, além de instante, canal, tamanho, RSSI e LQI.
- Preservar os bytes da trama para decidir depois quais campos e padrões são úteis. Não tentar descodificar conteúdo de aplicação cifrado.
- Ao encher o buffer de 128 tramas, parar a captura apenas no C6, ligar o Wi-Fi, enviar o CSV como anexo para ntfy e retomar a captura.
- Manter a captura em RAM, sem escrita contínua na flash.
- Não afetar o funcionamento dos dispositivos Zigbee da casa.

## Ciclo de funcionamento

1. O Wi-Fi fica desligado enquanto o C6 escuta o canal Zigbee 11.
2. Ao encher o buffer, o C6 pausa a captura e ativa o Wi-Fi.
3. Envia as tramas e os metadados como um CSV anexo ao ntfy.
4. Só depois de uma resposta HTTP de sucesso limpa o buffer, desliga o Wi-Fi e retoma a captura.
5. Se a ligação ou o envio falhar, mantém o bloco em RAM e tenta novamente após cinco minutos.

## Limites técnicos

- O componente Zigbee normal do ESPHome transforma o ESP32 num dispositivo da rede; não é um sniffer passivo. O Snifzig usa um componente externo ESPHome/C++ com a API IEEE 802.15.4.
- O ESP32-C6 partilha a rádio entre Wi-Fi e 802.15.4. Durante cada envio ntfy, este C6 deixa de ouvir Zigbee por alguns segundos; a rede Zigbee continua ativa.
- O driver entrega o cabeçalho MAC e o payload MAC sem o FCS; RSSI e LQI são metadados separados. O conteúdo de aplicação Zigbee pode estar cifrado.
- O buffer atual tem capacidade inicial para 128 tramas. Ao faltar energia ou reiniciar antes do envio, o bloco em RAM perde-se.
- A placa é a Seeed Studio XIAO ESP32-C6 e o firmware seleciona a antena cerâmica integrada.
- As credenciais Wi-Fi ficam em `secrets.yaml` local, ignorado pelo Git. O endereço e tópico ntfy devem ser definidos apenas na configuração local do ESPHome.

## Estado

Existe uma configuração ESPHome com captura, buffer em RAM, alternância Wi-Fi e envio CSV para ntfy. A implementação ainda não foi compilada nem validada na placa.

## Próximo passo

Definir localmente as credenciais Wi-Fi e o endereço/tópico ntfy, compilar e instalar no XIAO, depois observar o primeiro envio. A partir do volume real de dados ajustamos o tamanho do lote e a frequência.

Ver [docs/ROADMAP.md](docs/ROADMAP.md) para as etapas detalhadas.
