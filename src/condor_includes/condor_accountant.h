#ifndef _Condor_Accountant_H_
#define _Condor_Accountant_H_

#include <iostream.h>

#include "condor_classad.h"

#include "HashTable.h"
#include "MyString.h"
#include "Set.h"
#include "TimeClass.h"

class Accountant {

public:

  //--------------------------------------------------------
  // User Functions
  //--------------------------------------------------------

  double GetPriority(const MyString& CustomerName); // get priority for a customer
  void SetPriority(const MyString& CustomerName, double Priority); // set priority for a customer

  void AddMatch(const MyString& CustomerName, ClassAd* ResourceAd); // Add new match
  void RemoveMatch(const MyString& ResourceName); // remove a match

  void UpdatePriorities(); // update all the priorities

  void CheckMatches(ClassAdList& ResourceList);  // Remove matches that are not claimed

  //--------------------------------------------------------
  // Misc public functions
  //--------------------------------------------------------

  static int HashFunc(const MyString& Key, int TableSize) {
    int count=0;
    int length=Key.Length();
    for(int i=0; i<length; i++) count+=Key[i];
    return (count % TableSize);
  }
  
  Accountant(int MaxCustomers=1024, int MaxResources=1024);
                                                
protected:

  //--------------------------------------------------------
  // Protected Methods
  //--------------------------------------------------------

  void AddMatch(const MyString& CustomerName, const MyString& ResourceName, const Time& T); 
  void RemoveMatch(const MyString& ResourceName, const Time& T);

  void LoadState(); // Save to file
  void SaveState(); // Read from file

private:

  //--------------------------------------------------------
  // Configuration variables
  //--------------------------------------------------------

  double MinPriority;        // Minimum priority (if no resources used)
  double Epsilon;            // used to compare priority to zero
  double HalfLifePeriod;     // The time in sec in which the priority is halved by aging
  MyString PriorityFileName; // Name of priority file
  MyString MatchFileName;    // Name of Match file

  //--------------------------------------------------------
  // Internal data types
  //--------------------------------------------------------

  struct CustomerRecord {
    double Priority;
    double UnchargedTime;
    Set<MyString> ResourceNames;
    CustomerRecord() { Priority=UnchargedTime=0; }
  };

  struct ResourceRecord {
    MyString CustomerName;
    ClassAd* Ad;
    Time StartTime;
    ResourceRecord() { Ad=NULL; }
    ~ResourceRecord() { if (Ad) delete Ad; }
  };

  //--------------------------------------------------------
  // Data members
  //--------------------------------------------------------

  HashTable<MyString, CustomerRecord*> Customers;
  HashTable<MyString, ResourceRecord*> Resources;

  Time LastUpdateTime;

  //--------------------------------------------------------
  // Utility functions
  //--------------------------------------------------------

  static MyString GetResourceName(ClassAd* Resource);
  static int NotClaimed(ClassAd* ResourceAd);
  static void WriteLogEntry(ofstream& os, int AddMatch, const MyString& CustomerName, const MyString& ResourceName, const Time& T);
  void LogAction(int AddMatch, const MyString& CustomerName, const MyString& ResourceName, const Time& T);

};

#endif
