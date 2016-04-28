#include <cstdio>
#include <cstdlib>

#include "qs_perf_test.h"
#include "stats.h"
#include "ndds_namespace_cpp.h"

#ifdef RTI_WIN32
  #include <ctime>
  #include "gettimeofday.h"
#else
  #include <sys/time.h>
#endif

const int MOD = 10000;
const int CALC_MOD = 1;
const int WRITE_BURST = 5;
int WARMUP = 100;
int SAMPLE = 0;

#ifdef WIN32
#define getpid GetCurrentProcessId
#endif

int operator - (const timeval & end, const timeval & begin)
{
  int end_usec = end.tv_sec * (int) 1000 * 1000 + end.tv_usec;
  int begin_usec = begin.tv_sec * (int) 1000 * 1000 + begin.tv_usec;
  return end_usec - begin_usec; 
}

class PerfHelloWorldPubListener : public DDS::DataWriterListener {
    int accept_count;
    int reject_count;
    int count;
  public:
    bool foundQS;

    virtual void on_publication_matched(
            DDSDataWriter *writer, const DDS::PublicationMatchedStatus &status) 
    {
      DDS::SubscriptionBuiltinTopicData subscriptionData;
      DDS::ReturnCode_t retcode;
        
        if (status.current_count_change > 0) {
            retcode = writer->get_matched_subscription_data (
                    subscriptionData, 
                    status.last_subscription_handle);
            
            if (retcode == DDS_RETCODE_OK) {
                if (subscriptionData.service.kind == DDS_QUEUING_SERVICE_QOS ) {
                    foundQS = true;
                }
            }
        }
    }
     
    virtual void on_application_acknowledgment(DDSDataWriter *writer, const DDS::AcknowledgmentInfo &info)
    {
        if (info.valid_response_data) 
        {
            count++;
            if (info.response_data.value[0] == 1) {
                accept_count++;
            } else {
                reject_count++;
            }
            
           if( (SAMPLE!=0) && (count % SAMPLE == 0))
            {
              printf("Accepted %d and rejected %d.\n", accept_count, reject_count);
            }
        }
    }
    
    PerfHelloWorldPubListener() {
        foundQS = false;
        count = 0;
        accept_count = 0;
        reject_count = 0;
    }
};

struct PerfReaderListener : public DDSDataReaderListener
{
  reflex::sub::DataReader<PerfHelloWorld> dr;
  int count;

  public:
  bool foundQS;
  bool done;
  struct timeval time_start, time_end, prev;
  Stats latency_hist;

  void on_data_available(DDSDataReader * reader) override
  {
    DDS_AckResponseData_t responseData;
    if(dr.underlying())
    {
      connext::LoanedSamples<DDS_DynamicData> loan = dr.take();
      responseData.value.ensure_length(1,1);
      responseData.value[0] = 1;

      for (const connext::SampleRef<DDS_DynamicData> &sref : loan)
      {
        if (sref.info().valid_data)
        {
          PerfHelloWorld sample;
          count++;
          if(count == WARMUP)
          {
            gettimeofday(&time_start, NULL);
            prev = time_start;
          }
          if((count > WARMUP) && (count % CALC_MOD == 0)) 
          {
            reflex::read_dynamicdata(sample, sref.data());

            struct timeval recv_ts;
            gettimeofday(&recv_ts, NULL);
            long diff = (recv_ts.tv_sec*1000*1000 + recv_ts.tv_usec) - sample.timestamp;
            latency_hist.insert(diff);
          }
          if ((count % MOD == 0) && (count % CALC_MOD == 0))
          {
            struct timeval current;
            gettimeofday(&current,NULL);
            std::cout << "messageId = " << sample.messageId
                      << ". Current throughput is " 
                      << 1000.0*1000.0*MOD/(current-prev) << "\n";
            prev = current;
          }
          if(count == SAMPLE+WARMUP)  //last message.
          {
            gettimeofday(&time_end,NULL);
            done = true;
          }
        }
        if (dr.underlying()->acknowledge_all(responseData) != DDS_RETCODE_OK)
          std::cout<<"acknowledge_all error"<<std::endl;
      }
    }
  }

  void on_subscription_matched(DDSDataReader* reader, 
      const DDS::SubscriptionMatchedStatus& status)
  {
    DDS_PublicationBuiltinTopicData publicationData;
    DDS_ReturnCode_t retcode;
    if (status.current_count_change > 0) {
      retcode = reader->get_matched_publication_data (
          publicationData,
          status.last_publication_handle);

      if (retcode == DDS_RETCODE_OK) {
        if (publicationData.service.kind == DDS_QUEUING_SERVICE_QOS ) { 
          foundQS = true;
        }   
      }   
    }   
  }

  void set_reflex_datareader(reflex::sub::DataReader<PerfHelloWorld> reader)
  {
    dr = reader;
  }

  PerfReaderListener()
  {
    time_start.tv_sec = 0;
    time_start.tv_usec = 0;
    time_end.tv_sec = 0;
    time_end.tv_usec = 0;
    foundQS= false;
    count = 0;
    done = false;
  }
};

void getReaderGuidExpr(char * readerGuidExpr, int id)
{
  snprintf(readerGuidExpr, 255, "%s%030x%02x)",
           "@related_reader_guid.value = &hex(", getpid(), id);
}

void qs_perf_subscriber(int domain_id, int samples, int id)
{
  char topicName[255], readerGuidExpr[255];
  SAMPLE = samples;
  DDS_DataReaderQos readerQos;
  DDS_StringSeq cftParams;
  DDSTopic *topic = NULL;
  DDS_ReturnCode_t retcode = DDS_RETCODE_OK;

  DDSDomainParticipant * participant = DDSDomainParticipantFactory::get_instance()->
    create_participant(
        domain_id, DDS_PARTICIPANT_QOS_DEFAULT,
        NULL, DDS_STATUS_MASK_NONE);
  if (participant == NULL) {
    throw std::runtime_error("Unable to create participant");
  }

  participant->get_default_datareader_qos(readerQos);
  retcode = DDSPropertyQosPolicyHelper::assert_property(
      readerQos.property,
      "dds.data_reader.shared_subscriber_name",
      "SharedSubscriber",
      DDS_BOOLEAN_TRUE);
  if (retcode != DDS_RETCODE_OK) {
    printf("assert_property error %d\n", retcode);
    return ;

  }
  PerfReaderListener reader_listener;
 
  reflex::SafeTypeCode<PerfHelloWorld>
    stc(reflex::make_typecode<PerfHelloWorld>()); 

  //register the type
  DDSDynamicDataTypeSupport obj(stc.get(),DDS_DYNAMIC_DATA_TYPE_PROPERTY_DEFAULT);
  if(obj.register_type(participant,"PerfHelloWorld") != DDS_RETCODE_OK)
  {
    std::cout <<"\n register_type error \n";
    return;
  }

  snprintf(topicName,255,"%s@%s", "PerfHelloWorldTopic", "SharedSubscriber");

  topic = participant->create_topic(
      topicName,
      "PerfHelloWorld", DDS_TOPIC_QOS_DEFAULT, NULL ,
      DDS_STATUS_MASK_NONE);
  if (topic == NULL)
  {	
    std::cout <<"\n topic error! \n";
    return;
  } 
  getReaderGuidExpr(readerGuidExpr, id);

  DDSContentFilteredTopic * cftTopic = participant->create_contentfilteredtopic(
      topicName,
      topic,
      readerGuidExpr,
      cftParams);
  if ( cftTopic == NULL)
  {
    std::cout <<"\n cft error\n";
    return;
  }

  reflex::sub::DataReader<PerfHelloWorld>
    datareader(reflex::sub::DataReaderParams(participant)
        .topic_name(topicName)
        .listener(&reader_listener)
        .datareader_qos(readerQos));

  reader_listener.set_reflex_datareader(datareader);

  printf("Waiting to discover SharedReaderQueue ...\n");

  while (!reader_listener.foundQS) 
  {
    DDS_Duration_t period { 0, 100 * 1000 * 1000 };
    NDDSUtility::sleep(period);
  }

  std::cout <<"Found SharedReaderQueue!\n";
  while(!reader_listener.done)
  {
    DDS_Duration_t poll_period = { 2, 0 };
    NDDSUtility::sleep(poll_period);
  }
  
  std::cout << "Reader Id = " << id << std::endl;
  reader_listener.latency_hist.sort_keys();
  //reader_listener.latency_hist.print_histogram();
  std::cout << "latency average = " 
            << reader_listener.latency_hist.average() << std::endl;
  std::cout << "latency stddev = " 
            << reader_listener.latency_hist.stddev() << std::endl;
  std::cout << "latency 90 percentile = " 
            << reader_listener.latency_hist.percentile(0.90) << std::endl;
  std::cout << "latency 99 percentile = " 
            << reader_listener.latency_hist.percentile(0.99) << std::endl;
  std::cout << "latency 99.9 percentile = " 
            << reader_listener.latency_hist.percentile(0.999) << std::endl;
  std::cout << "latency 99.99 percentile = " 
            << reader_listener.latency_hist.percentile(0.9999) << std::endl;
  std::cout << "latency 99.999 percentile = " 
            << reader_listener.latency_hist.percentile(0.99999) << std::endl;
  std::cout << "latency max = " 
            << reader_listener.latency_hist.get_max() << std::endl;
  std::cout << "Throughput = " 
            << 1000.0*1000.9*reader_listener.count/(reader_listener.time_end - reader_listener.time_start)
            << std::endl;
  std::cout << reader_listener.latency_hist.average() << " "  
            << reader_listener.latency_hist.stddev() << " " 
            << reader_listener.latency_hist.percentile(0.90) << " "
            << reader_listener.latency_hist.percentile(0.99) << " "
            << reader_listener.latency_hist.percentile(0.999) << " "
            << reader_listener.latency_hist.percentile(0.9999) << " "
            << reader_listener.latency_hist.percentile(0.99999) << " "
            << reader_listener.latency_hist.get_max() << "\n";
}

double scale_sleep_period_usec(int hz) 
{
  double scale_factor = 1.30;
  scale_factor /= WRITE_BURST;
  return 1000.0/(hz*scale_factor)*1000;
}

void qs_perf_publisher(int domain_id, int samples, int no_readers, int hertz)
{
  DDS_ReturnCode_t         rc = DDS_RETCODE_OK;
  DDSDomainParticipant *   participant = NULL;
  DDS_DynamicDataTypeProperty_t props;
  double expected_sleep_period_usec = scale_sleep_period_usec(hertz);
  DDS_Duration_t loop_period { 0, (unsigned int) expected_sleep_period_usec * 1000 };
  DDS_Duration_t end_wait{ 3, 0 };
  const char *topic_name = "PerfHelloWorldTopic";
  SAMPLE = samples * no_readers;
  WARMUP = WARMUP * no_readers;
  Stats sleep_hist;
  struct timeval begin = {0}, end = {0};

  PerfHelloWorldPubListener writerListener;
  participant = DDSDomainParticipantFactory::get_instance()->
  	    create_participant(
               domain_id,DDS_PARTICIPANT_QOS_DEFAULT,
               NULL , DDS_STATUS_MASK_NONE);
  
  if (participant == NULL) {
    std::cerr << "! Unable to create DDS domain participant" 
              << std::endl;
    return;
  } 

  PerfHelloWorldPubListener listener;

  reflex::pub::DataWriter<PerfHelloWorld>
     writer(reflex::pub::DataWriterParams(participant)
             .topic_name(topic_name)
             .listener_statusmask(DDS_STATUS_MASK_ALL)
             .listener(&listener));

   //Wait for Queuing Service discovery 
   std::cout <<"\nWaiting to discover SharedReaderQueue ...\n";

  while (!listener.foundQS) {
     NDDSUtility::sleep(loop_period);
  }

  std::cout<<"\nSharedReaderQueue discovered...\n";

  struct timeval prev = {0};
  gettimeofday(&prev, NULL);

  for(int i = 1;i <= (SAMPLE+WARMUP); i++)
  {
     PerfHelloWorld obj;
     obj.messageId = i;
     if (i == WARMUP) {
       gettimeofday(&begin, NULL);
     }

     struct timeval ts;
     gettimeofday(&ts, NULL);
     obj.timestamp = ts.tv_sec*1000*1000 + ts.tv_usec;

     rc = writer.write(obj);

     if((i % MOD) == 0)
     {
       double elapsed_time_usec = (double) (ts-prev);
       prev = ts;
       std::cout << "Wrote " << i 
                 << ". tput = " << 1000.0*1000.0*MOD/elapsed_time_usec << "\n";
     }

     if (rc != DDS_RETCODE_OK) 
     {
       std::cerr << "Write error = "
                 << reflex::detail::get_readable_retcode(rc)
                 << std::endl;
     }

     if(i % WRITE_BURST == 0)
     {
       struct timeval start_sleep, end_sleep;
       gettimeofday(&start_sleep, NULL);
       NDDSUtility::sleep(loop_period);
       gettimeofday(&end_sleep,NULL);
       sleep_hist.insert(end_sleep-start_sleep); 
     }
  }
  
  gettimeofday(&end,NULL);

  NDDSUtility::sleep(end_wait);
  sleep_hist.sort_keys();

  std::cout << "Throughput is "<< 1000.0*1000.0*SAMPLE / (end - begin) << std::endl;
  std::cout << "sleep expected period in usec = " << expected_sleep_period_usec << std::endl;
  std::cout << "sleep average = " << sleep_hist.average() 
            << " stddev = " << sleep_hist.stddev() << std::endl;
  std::cout << "sleep 90 percentile = " << sleep_hist.percentile(0.90) << std::endl;
  std::cout << "sleep 99 percentile = " << sleep_hist.percentile(0.99) << std::endl;
  std::cout << "sleep 99.9 percentile = " << sleep_hist.percentile(0.999) << std::endl;
  std::cout << "sleep max = " << sleep_hist.get_max() << std::endl;

}

