# Plano de trabalho

## 0. Provar a captura passiva dentro do ESPHome

O componente externo em `components/snifzig_capture` e a configuração `snifzig.yaml` implementam a captura no canal 11.

Critérios:
- O C6 recebe tramas da rede Zigbee existente sem aderir a ela.
- O sniffer não transmite nem altera a rede Zigbee.
- O Wi-Fi fica desligado durante a captura.
- São guardados os bytes recebidos do cabeçalho MAC e payload MAC, mais instante, canal, tamanho, RSSI e LQI quando disponibilizados pelo driver.

Ainda falta compilar e confirmar o comportamento na placa.

## 1. Capturar e caracterizar

A primeira versão produz um CSV com uma linha por trama: bytes recebidos em hexadecimal e campos de receção separados. A análise decidirá quais campos e padrões são úteis.

Confirmar se os endereços disponíveis podem ser associados aos dispositivos do Zigbee2MQTT e o que é possível aprender sem a chave da rede. Não descodificar conteúdo de aplicação.

## 2. Wi-Fi e ntfy

A configuração e o componente implementam o ciclo:

1. Capturar e armazenar até 128 tramas em RAM.
2. Pausar a captura quando o buffer encher.
3. Ativar Wi-Fi e enviar o CSV como anexo com HTTP PUT para ntfy.
4. Limpar o buffer apenas após resposta HTTP de sucesso.
5. Desativar Wi-Fi e retomar a captura.

Se a ligação ou o envio falhar, o bloco fica pendente em RAM e o dispositivo tenta de novo após cinco minutos. Ainda falta compilar e observar o primeiro envio real. O buffer perde-se se houver perda de energia antes do envio.

## 3. Analisar a rede da casa

Para cada captura, calcular contagens e distribuições de RSSI/LQI por endereço de origem e intervalo temporal. Comparar capturas feitas no mesmo local e, se o Zigbee2MQTT mudar do canal 11 para o 25, comparar os resultados antes e depois da migração.

Durante cada envio ntfy, o C6 deixa de ouvir algumas tramas; a rede Zigbee da casa continua ativa.

## Decisões confirmadas

- Placa: Seeed Studio XIAO ESP32-C6.
- Canal inicial: 11.
- Antena inicial: cerâmica integrada.
- Captura: bytes da trama recebida mais metadados; a análise decidirá depois quais campos importam.
- Armazenamento: RAM, sem escrita contínua na flash.
- Arquitetura: ESPHome com componente externo; sem aplicação autónoma ESP-IDF.
- O Wi-Fi só é ativado para publicar um lote ntfy.

## Por decidir durante a implementação

- Aumentar ou não o lote de 128 tramas, após medir a taxa real.
- Definir o tempo máximo de captura entre envios, se não houver tráfego suficiente para encher o buffer.
- Rever a codificação CSV e tamanho de anexos após o primeiro ficheiro real.
- Confirmar localmente o endereço do servidor ntfy, o tópico e se é necessária autenticação; não guardar o endereço pessoal nem credenciais no Git.
