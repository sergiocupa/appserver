using System;
using System.Text;
using System.Windows.Forms;


namespace HttpClientTester
{
    public partial class Form1 : Form
    {

        void Received(byte[] data)
        {
            richTextBox2.Invoke((Action)(() => 
            {
                var txt = Encoding.UTF8.GetString(data);
                richTextBox2.AppendText(txt);
                richTextBox2.AppendText("\r\n");
            }));
        }


        private void GetLength()
        {
            try
            {
                var content = richTextBox1.Text.Replace("\r\n", "\n").Replace("\n","\r\n");

                if (content.Contains(" HTTP/") && content.Contains("\r\nContent-Length:"))
                {
                    var ix = content.IndexOf("\r\n\r\n");
                    if (ix > 0)
                    {
                        var json = content.Substring(ix + 4);
                        var bytes = Encoding.UTF8.GetByteCount(json);
                        label4.Text = bytes.ToString();
                    } 
                }
            }
            catch (Exception)
            {
            }
        }


        private void button2_Click(object sender, EventArgs e)
        {
            try
            {
                if (Client != null)
                {
                    var content = richTextBox1.Text.Replace("\r\n", "\n").Replace("\n", "\r\n");
                    var bytes = Encoding.UTF8.GetBytes(content);
                    Client.Send(bytes);
                }
                else
                {
                    MessageBox.Show("Não foi estabelicido conexão");
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message);
            }
        }

        private void button1_Click(object sender, EventArgs e)
        {
            try
            {
                if (Client == null)
                {
                    Client = new TcpClientConnection(textBox1.Text, Received);
                    Client.Open();
                    button1.Enabled = false;
                    button3.Enabled = true;
                }
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message);
            }
        }

        private void button3_Click(object sender, EventArgs e)
        {
            try
            {
                if (Client != null)
                {
                    Client.Close();
                    button1.Enabled = true;
                    button3.Enabled = false;
                }
                Client = null;
            }
            catch (Exception ex)
            {
                MessageBox.Show(ex.Message);
            }
        }

        private void Form1_FormClosing(object sender, FormClosingEventArgs e)
        {
            try
            {
                if (Client != null)
                {
                    Client.Close();
                }
            }
            catch (Exception ex)
            {

            }
        }

        private void richTextBox1_TextChanged(object sender, EventArgs e)
        {
            GetLength();
        }


        TcpClientConnection Client;
        string json;


        public Form1()
        {
            InitializeComponent();

            richTextBox1.Text = 
            "POST /api/usuarios HTTP/1.1\r\n" +
            "Host: exemplo.com\r\n" +
            "Content-Type: application/json\r\n" +
            "Content-Length: 26\r\n" +
            "\r\n" +
            "{\"nome\":\"Joao\",\"idade\":30}";

            GetLength();
        }

       
    }
}
